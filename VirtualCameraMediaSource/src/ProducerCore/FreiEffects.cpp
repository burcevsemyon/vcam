// Backend помех на frei0r-плагинах (сборка DLL — third_party/frei0r/build.ps1).
//
// Загрузка: LoadLibrary по полному пути; порядок каталогов —
//   1) тестовый override (SetSearchDir),
//   2) <каталог хоста>\frei0r\ (раскладка build и инсталлятора),
//   3) <каталог хоста>,
//   4) FREI0R_PATH (через ';' — так велит спека для Windows).
// Имена: rgbnoise.dll, scanline0r.dll, rgbsplit0r.dll, glitch0r.dll.
//
// Факты апстрима (пин 5378516, проверены чтением исходников):
// - rgbnoise/rgbsplit0r/glitch0r — RGBA8888 (byte0=R), только f0r_update;
//   scanline0r (C++ frei0r.hpp) — BGRA8888, 0 params, есть update и update2.
//   Наши DLL экспортируют и update, и update2 (C-плагинам update2 добит
//   шимом-форвардом в build.ps1); хост предпочитает update2.
// - Параметры (все double 0..1, менять каждый кадр можно и нужно):
//   rgbnoise[0] noise: byteNoise = v*gauss*127 → 0.0 точный no-op.
//   rgbsplit0r[0] vert c=0.5, [1] horiz c=0.5; shift=(v-0.5)*dim/8.
//   glitch0r[0] freq→0..100%, [1] block→1..h, [2] shift→1..w,
//     [3] color→0..5; freq=0 → все строки passthrough (memcpy).
// - Маппинг level (монотонно, level 0 = пропуск плагина = точный no-op).
//   Характер — «сочный frei0r» (фаза vcam-effects-character, CPU — эталон,
//   НЕ трогаем): каждый эффект на 100 заметно сильнее CPU-аналога.
//   noise:     v = L/100, ДВА прохода (σ_total ≈ σ√2; 100 → лютая статика).
//     Параметр в спеке 0..1 — за неё не выходим, злость берём повтором.
//   scanlines: params нет — пост-обработка в обёртке (снапшот нечётных до
//     плагина, stageC): L<=50 — blend-back к исходнику
//     out = (pre*(50-L) + plugin*L + 25)/50 (25 → ~×0.75, 50 → ×0.5);
//     L>50 — дополнительное затемнение out*extra/100,
//     extra = 100−70*(L−50)/50 (100 → ×0.3 → итог ×0.15: CRT-провал,
//     вдвое глубже CPU ×0.35 — backend'ы разведены визуально).
//   rgbsplit:  vert 0.5+L*0.04 (100 → dy≈3px@720, лёгкий крен),
//     horiz 0.5+L*0.25 → dx = L/100*w/32 (100 → 40px@1280 — втрое шире CPU;
//     320px кадр: dx=10, детерминировано: FP-точные 0.25/0.125/0.0625).
//   tracking:  freq 0.15+0.85*L (100 → 100% строк), block 0.05+0.30*L
//     (100 → 0.35 ≈ 252px@720 — крупные блоки), shift 0.05+0.25*L
//     (100 → 0.30 ≈ 384px@1280), color 0.2+0.8*L (100 → 5/5 безумия).
// Порядок цепочки — как CPU: rgbsplit → tracking(glitch0r) → noise → scanlines.
//
// Потокобезопасность: один мьютекс на весь вызов (заодно сериализует
// rand() внутри rgbnoise/glitch0r). AV в чужом коде ловим SEH и помечаем
// плагин битым (выгружаем) → kNeedCpu, кадр НЕ тронут (работаем на копиях,
// в bgrx пишем только после успеха всей цепочки — атомарно).
// Выравнивание: спека требует 16 байт — stage-буферы через _aligned_malloc.

#include "FreiEffects.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <windows.h>

namespace vcam::effects::frei {
namespace {

// --- Минимальные копии структур frei0r.h (чтобы ProducerCore не зависел
// --- от скачанных хедеров апстрима при сборке) ---
constexpr int kF0rPluginFilter = 0;
constexpr int kF0rColorBgra = 0;
constexpr int kF0rColorRgba = 1;
constexpr int kF0rColorPacked = 2;

struct F0rPluginInfo {
    const char* name;
    const char* author;
    int pluginType;
    int colorModel;
    int frei0rVersion;
    int majorVersion;
    int minorVersion;
    int numParams;
    const char* explanation;
};

struct F0rParamInfo {
    const char* name;
    int type;
    const char* explanation;
};

using F0rInstance = void*;
using F0rParam = void*;
using FnInit = int (*)();
using FnDeinit = void (*)();
using FnGetPluginInfo = void (*)(F0rPluginInfo*);
using FnGetParamInfo = void (*)(F0rParamInfo*, int);
using FnConstruct = F0rInstance (*)(unsigned int, unsigned int);
using FnDestruct = void (*)(F0rInstance);
using FnSetParam = void (*)(F0rInstance, F0rParam, int);
using FnUpdate = void (*)(F0rInstance, double, const uint32_t*, uint32_t*);
using FnUpdate2 = void (*)(F0rInstance, double, const uint32_t*, const uint32_t*,
                            const uint32_t*, uint32_t*);

struct LoadedPlugin {
    HMODULE dll = nullptr;
    FnInit init = nullptr;
    FnDeinit deinit = nullptr;
    FnConstruct construct = nullptr;
    FnDestruct destruct = nullptr;
    FnSetParam setParam = nullptr;
    FnUpdate update = nullptr;
    FnUpdate2 update2 = nullptr;
    int colorModel = -1;
    int numParams = -1;
    bool inited = false;
    bool broken = false;
    std::wstring path;
};

// Эффект → DLL + ожидания по параметрам (валидация после загрузки).
struct EffectDef {
    const wchar_t* dllName;
    int minParams; // scanline0r: ровно 0 (ровно; остальные — минимум)
    bool exactParams;
};
constexpr int kFxNoise = 0;
constexpr int kFxScanlines = 1;
constexpr int kFxRgbSplit = 2;
constexpr int kFxTracking = 3;
constexpr int kFxCount = 4;
const EffectDef kEffects[kFxCount] = {
    {L"rgbnoise.dll", 1, false},
    {L"scanline0r.dll", 0, true},
    {L"rgbsplit0r.dll", 2, false},
    {L"glitch0r.dll", 4, false},
};

struct InstanceKey {
    int fx = -1;
    uint32_t w = 0;
    uint32_t h = 0;
    bool operator<(const InstanceKey& o) const {
        if (fx != o.fx) return fx < o.fx;
        if (w != o.w) return w < o.w;
        return h < o.h;
    }
};

struct State {
    std::mutex mutex;
    LoadedPlugin plugins[kFxCount];
    bool pluginsProbed[kFxCount] = {};
    std::map<InstanceKey, F0rInstance> instances;
    uint8_t* stageA = nullptr; // 16-байт, BGRX-layout stage
    uint8_t* stageB = nullptr;
    uint8_t* stageC = nullptr; // снапшот кадра до scanline0r (blend-back L<=50)
    size_t stageBytes = 0;
    std::wstring searchOverride;
};

State& FreiState() {
    static State s;
    return s;
}

std::atomic<bool> g_cpuFallback{false};

// SEH-барьер: в __try-функции только POD-контекст.
using Thunk = void (*)(void*);

bool GuardedCall(Thunk fn, void* ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct ConstructCtx {
    LoadedPlugin* lp;
    unsigned int w;
    unsigned int h;
    F0rInstance inst;
};

void ConstructThunk(void* p) {
    auto* c = static_cast<ConstructCtx*>(p);
    c->inst = c->lp->construct(c->w, c->h);
}

struct UpdateCtx {
    LoadedPlugin* lp;
    F0rInstance inst;
    double t;
    const uint32_t* in;
    uint32_t* out;
    bool ok;
};

void UpdateThunk(void* p) {
    auto* c = static_cast<UpdateCtx*>(p);
    if (c->lp->update2) {
        c->lp->update2(c->inst, c->t, c->in, nullptr, nullptr, c->out);
    } else {
        c->lp->update(c->inst, c->t, c->in, c->out);
    }
    c->ok = true;
}

// К3: прямой вызов чужого кода без таймаута вешал worker host-кадра навсегда.
// Крутим UpdateThunk на отдельной нити, ждём ~500 мс, дальше fail-open.
// Возврат: 1 = ok, 0 = упало быстро (SEH/false, нить завершена — плагин можно
// безопасно выгружать через DropPluginLocked), -1 = ТАЙМАУТ (нить ещё внутри
// чужого кода — выгружать/destruct/FreeLibrary НЕЛЬЗЯ, только липкий broken
// без выгрузки; см. MarkPluginBrokenLocked). Контекст и promise — shared
// (detached-нить держит свои копии), висячих записей в стек вызывающего нет;
// выходной буфер при таймауте не трогаем (остаток кадра — через CPU fallback).
int UpdateWithTimeout(LoadedPlugin* lp, F0rInstance inst, double t,
                      const uint32_t* in, uint32_t* out, int timeoutMs = 500) {
    auto job = std::make_shared<UpdateCtx>(UpdateCtx{lp, inst, t, in, out, false});
    auto pr = std::make_shared<std::promise<bool>>();
    std::future<bool> fu = pr->get_future();
    try {
        std::thread([job, pr] {
            bool ok = GuardedCall(UpdateThunk, job.get()) && job->ok;
            try {
                pr->set_value(ok);
            } catch (...) {
            }
        }).detach();
    } catch (...) {
        return 0;
    }
    if (fu.wait_for(std::chrono::milliseconds(timeoutMs)) != std::future_status::ready)
        return -1; // timeout: detached-нить держит job/pr — UAF нет
    try {
        return fu.get() ? 1 : 0;
    } catch (...) {
        return 0;
    }
}

struct DestructCtx {
    LoadedPlugin* lp;
    F0rInstance inst;
};

void DestructThunk(void* p) {
    auto* c = static_cast<DestructCtx*>(p);
    c->lp->destruct(c->inst);
}

struct DeinitCtx {
    FnDeinit f;
};

void DeinitThunk(void* p) {
    static_cast<DeinitCtx*>(p)->f();
}

struct GetInfoCtx {
    FnGetPluginInfo f;
    F0rPluginInfo* out;
};

void GetInfoThunk(void* p) {
    auto* c = static_cast<GetInfoCtx*>(p);
    c->f(c->out);
}

struct InitCtx {
    FnInit f;
    int r;
};

void InitThunk(void* p) {
    auto* c = static_cast<InitCtx*>(p);
    c->r = c->f();
}

// Каталог хоста (= каталог exe; ProducerCore — static lib).
std::wstring HostDir() {
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    std::wstring s(buf, n);
    size_t slash = s.find_last_of(L"\\/");
    return (slash == std::wstring::npos) ? std::wstring() : s.substr(0, slash);
}

void SplitFrei0rPath(const std::wstring& env, std::vector<std::wstring>& out) {
    size_t start = 0;
    for (;;) {
        size_t semi = env.find(L';', start);
        std::wstring part = env.substr(start, semi == std::wstring::npos
                                                  ? std::wstring::npos
                                                  : semi - start);
        if (!part.empty()) out.push_back(part);
        if (semi == std::wstring::npos) break;
        start = semi + 1;
    }
}

// Полный путь к DLL эффекта ("" — не найден).
std::wstring FindDll(State& st, const wchar_t* dllName) {
    if (!st.searchOverride.empty()) {
        std::wstring p = st.searchOverride + L"\\" + dllName;
        DWORD a = GetFileAttributesW(p.c_str());
        return (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
                   ? p
                   : std::wstring();
    }
    std::vector<std::wstring> dirs;
    std::wstring hd = HostDir();
    if (!hd.empty()) {
        dirs.push_back(hd + L"\\frei0r");
        dirs.push_back(hd);
    }
    wchar_t env[32767] = {};
    if (GetEnvironmentVariableW(L"FREI0R_PATH", env, 32767) > 0)
        SplitFrei0rPath(env, dirs);
    for (const auto& d : dirs) {
        std::wstring p = d + L"\\" + dllName;
        DWORD a = GetFileAttributesW(p.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY))
            return p;
    }
    return std::wstring();
}

template <typename T>
T Proc(HMODULE dll, const char* name) {
    return reinterpret_cast<T>(GetProcAddress(dll, name));
}

// Загрузка + валидация одного плагина (мьютекс уже взят). false = нет/битый.
bool EnsurePluginLocked(State& st, int fx) {
    LoadedPlugin& lp = st.plugins[fx];
    if (lp.broken) return false;
    if (lp.dll) return true;
    if (st.pluginsProbed[fx]) return false; // уже искали и не нашли
    st.pluginsProbed[fx] = true;

    std::wstring path = FindDll(st, kEffects[fx].dllName);
    if (path.empty()) return false;
    HMODULE dll = LoadLibraryW(path.c_str());
    if (!dll) return false;

    FnInit init = Proc<FnInit>(dll, "f0r_init");
    FnDeinit deinit = Proc<FnDeinit>(dll, "f0r_deinit");
    FnGetPluginInfo getInfo = Proc<FnGetPluginInfo>(dll, "f0r_get_plugin_info");
    FnGetParamInfo getParam = Proc<FnGetParamInfo>(dll, "f0r_get_param_info");
    FnConstruct construct = Proc<FnConstruct>(dll, "f0r_construct");
    FnDestruct destruct = Proc<FnDestruct>(dll, "f0r_destruct");
    FnSetParam setParam = Proc<FnSetParam>(dll, "f0r_set_param_value");
    FnUpdate update = Proc<FnUpdate>(dll, "f0r_update");
    FnUpdate2 update2 = Proc<FnUpdate2>(dll, "f0r_update2");
    if (!init || !deinit || !getInfo || !getParam || !construct || !destruct ||
        !setParam || (!update && !update2)) {
        FreeLibrary(dll);
        return false;
    }
    // init один раз (SEH: чужой код).
    InitCtx ic{init, 0};
    if (!GuardedCall(InitThunk, &ic) || ic.r == 0) {
        FreeLibrary(dll);
        return false;
    }
    F0rPluginInfo pi{};
    GetInfoCtx gc{getInfo, &pi};
    if (!GuardedCall(GetInfoThunk, &gc)) {
        FreeLibrary(dll);
        return false;
    }
    if (pi.pluginType != kF0rPluginFilter || (pi.colorModel != kF0rColorBgra &&
        pi.colorModel != kF0rColorRgba && pi.colorModel != kF0rColorPacked)) {
        deinit();
        FreeLibrary(dll);
        return false;
    }
    const EffectDef& def = kEffects[fx];
    if (def.exactParams ? (pi.numParams != def.minParams)
                        : (pi.numParams < def.minParams)) {
        deinit();
        FreeLibrary(dll);
        return false;
    }
    lp.dll = dll;
    lp.init = init;
    lp.deinit = deinit;
    lp.construct = construct;
    lp.destruct = destruct;
    lp.setParam = setParam;
    lp.update = update;
    lp.update2 = update2;
    lp.colorModel = pi.colorModel;
    lp.numParams = pi.numParams;
    lp.inited = true;
    lp.path = path;
    return true;
}

void DropPluginLocked(State& st, int fx) {
    LoadedPlugin& lp = st.plugins[fx];
    for (auto it = st.instances.begin(); it != st.instances.end();) {
        if (it->first.fx == fx) {
            DestructCtx c{&lp, it->second};
            GuardedCall(DestructThunk, &c);
            it = st.instances.erase(it);
        } else {
            ++it;
        }
    }
    if (lp.dll) {
        if (lp.inited) {
            DeinitCtx c{lp.deinit};
            GuardedCall(DeinitThunk, &c);
        }
        FreeLibrary(lp.dll);
    }
    lp = LoadedPlugin{};
    lp.broken = true; // до ShutdownFrei не трогаем (липкий битый)
}

// К3: пометить плагин битым БЕЗ выгрузки (для таймаута UpdateWithTimeout:
// hung-нить ещё внутри update/dll — destruct/deinit/FreeLibrary по живому
// коду = AV). Инстанс и DLL утекают bounded (макс 4 плагина за процесс),
// повторных обращений не будет (EnsurePluginLocked смотрит broken первым).
// Stage-буферы при этом может дописывать hung-нить — остаток кадра идёт
// через CPU fallback, гонка принята как меньшее зло против вечного hang.
void MarkPluginBrokenLocked(State& st, int fx) {
    st.plugins[fx].broken = true;
}

// Instance под (w,h): кэш, пересоздание при смене размера (мьютекс взят).
// nullptr = construct упал/AV → плагин помечается битым.
F0rInstance EnsureInstanceLocked(State& st, int fx, uint32_t w, uint32_t h) {
    InstanceKey k{fx, w, h};
    auto it = st.instances.find(k);
    if (it != st.instances.end()) return it->second;
    // Старые размеры этого плагина — снести.
    for (auto j = st.instances.begin(); j != st.instances.end();) {
        if (j->first.fx == fx) {
            DestructCtx c{&st.plugins[fx], j->second};
            GuardedCall(DestructThunk, &c);
            j = st.instances.erase(j);
        } else {
            ++j;
        }
    }
    LoadedPlugin& lp = st.plugins[fx];
    ConstructCtx c{&lp, w, h, nullptr};
    if (!GuardedCall(ConstructThunk, &c) || !c.inst) {
        DropPluginLocked(st, fx);
        return nullptr;
    }
    st.instances[k] = c.inst;
    return c.inst;
}

// BGRX stage → layout плагина (RGBA: swap R<->B; BGRA/PACKED: копия).
void ToPluginLayout(const LoadedPlugin& lp, const uint8_t* src, uint8_t* dst,
                    size_t pixels) {
    if (lp.colorModel == kF0rColorRgba) {
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* s = src + i * 4u;
            uint8_t* d = dst + i * 4u;
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        }
    } else {
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* s = src + i * 4u;
            uint8_t* d = dst + i * 4u;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
}

// Layout плагина → BGRX stage (обратный swap + X=255; 4-й байт плагины
// могут портить — scanline0r множит и альфу на нечётных строках).
void FromPluginLayout(const LoadedPlugin& lp, const uint8_t* src, uint8_t* dst,
                      size_t pixels) {
    if (lp.colorModel == kF0rColorRgba) {
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* s = src + i * 4u;
            uint8_t* d = dst + i * 4u;
            d[0] = s[2];
            d[1] = s[1];
            d[2] = s[0];
            d[3] = 255;
        }
    } else {
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* s = src + i * 4u;
            uint8_t* d = dst + i * 4u;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
    }
}

struct SetParamCtx {
    LoadedPlugin* lp;
    F0rInstance inst;
    double v;
    int idx;
};

void SetParamThunk(void* p) {
    auto* c = static_cast<SetParamCtx*>(p);
    c->lp->setParam(c->inst, &c->v, c->idx);
}

void SetDouble(LoadedPlugin& lp, F0rInstance inst, int idx, double v) {
    SetParamCtx c{&lp, inst, v, idx};
    GuardedCall(SetParamThunk, &c);
}

} // namespace

FreiResult ApplyAnalog(uint8_t* bgrx, uint32_t w, uint32_t h,
                       const AnalogRequest& req) {
    if (!bgrx || w == 0 || h == 0) return FreiResult::kFailed;
    if (w > 16384 || h > 16384) return FreiResult::kFailed;
    // Спека frei0r: w/h — кратные 8 (у нас 1280×720 и нативы камеры такие же).
    if ((w & 7u) || (h & 7u)) return FreiResult::kNeedCpu;

    const size_t pixels = (size_t)w * h;
    const size_t bytes = pixels * 4u;

    // Какие эффекты реально вызывать (level 0 = пропуск = точный no-op).
    struct Job {
        int fx;
        int level;
    };
    Job jobs[kFxCount];
    int nJobs = 0;
    auto want = [&](bool on, int level, int fx) {
        if (on && level > 0) jobs[nJobs++] = Job{fx, level};
    };
    want(req.rgbSplit, req.rgbSplitLevel, kFxRgbSplit);
    want(req.tracking, req.trackingLevel, kFxTracking);
    want(req.noise, req.noiseLevel, kFxNoise);
    want(req.scanlines, req.scanlinesLevel, kFxScanlines);
    if (nJobs == 0) return FreiResult::kApplied; // нечего делать, кадр цел

    State& st = FreiState();
    std::unique_lock<std::mutex> lock(st.mutex);

    if (st.stageBytes < bytes || !st.stageA || !st.stageB || !st.stageC) {
        if (st.stageA) _aligned_free(st.stageA);
        if (st.stageB) _aligned_free(st.stageB);
        if (st.stageC) _aligned_free(st.stageC);
        st.stageA = (uint8_t*)_aligned_malloc(bytes, 16);
        st.stageB = (uint8_t*)_aligned_malloc(bytes, 16);
        st.stageC = (uint8_t*)_aligned_malloc(bytes, 16);
        if (!st.stageA || !st.stageB || !st.stageC) {
            if (st.stageA) _aligned_free(st.stageA);
            if (st.stageB) _aligned_free(st.stageB);
            if (st.stageC) _aligned_free(st.stageC);
            st.stageA = st.stageB = st.stageC = nullptr;
            st.stageBytes = 0;
            return FreiResult::kFailed;
        }
        st.stageBytes = bytes;
    }
    uint8_t* cur = st.stageA;
    uint8_t* nxt = st.stageB;
    std::memcpy(cur, bgrx, bytes);

    for (int j = 0; j < nJobs; ++j) {
        const int fx = jobs[j].fx;
        const int level = jobs[j].level;
        const double L = (double)level / 100.0;
        if (!EnsurePluginLocked(st, fx)) {
            g_cpuFallback.store(true, std::memory_order_relaxed);
            return FreiResult::kNeedCpu;
        }
        LoadedPlugin& lp = st.plugins[fx];
        F0rInstance inst = EnsureInstanceLocked(st, fx, w, h);
        if (!inst) {
            g_cpuFallback.store(true, std::memory_order_relaxed);
            return FreiResult::kNeedCpu;
        }
        // Параметры каждый кадр (hot-swap уровней без переоткрытия).
        // Характерные режимы (фаза character): noise — два прохода,
        // scanlines — пост-обработка ниже (снапшот нужен ДО плагина).
        int passes = 1;
        const bool needSnap = (fx == kFxScanlines && level <= 50);
        if (needSnap) std::memcpy(st.stageC, cur, bytes);
        switch (fx) {
        case kFxNoise:
            SetDouble(lp, inst, 0, L);
            passes = 2; // злее: повторный проход тем же v (σ√2)
            break;
        case kFxScanlines:
            break; // params нет — пост-обработка после плагина
        case kFxRgbSplit:
            SetDouble(lp, inst, 0, 0.5 + L * 0.04); // vertical: лёгкий крен
            SetDouble(lp, inst, 1, 0.5 + L * 0.25); // horiz: dx=40px@1280
            break;
        case kFxTracking:
            SetDouble(lp, inst, 0, 0.15 + 0.85 * L); // freq (100 → все строки)
            SetDouble(lp, inst, 1, 0.05 + 0.30 * L); // block (100 → 0.35h)
            SetDouble(lp, inst, 2, 0.05 + 0.25 * L); // shift (100 → 0.30w)
            SetDouble(lp, inst, 3, 0.20 + 0.80 * L); // color (100 → 5/5)
            break;
        }
        // Стадия: BGRX(cur) → layout плагина(nxt) → update(in=nxt, out=cur)
        // → BGRX(cur→nxt); nxt — текущий. В bgrx пишем только после успеха
        // всей цепочки (атомарно).
        for (int p = 0; p < passes; ++p) {
            ToPluginLayout(lp, cur, nxt, pixels);
            // К3: прямой вызов без таймаута вешал worker навсегда — ждём
            // ~500 мс, дальше fail-open на CPU (kNeedCpu). Таймаут (-1) —
            // только липкий broken БЕЗ выгрузки (hung-нить ещё внутри).
            const int ur = UpdateWithTimeout(
                &lp, inst, req.timeSec,
                reinterpret_cast<const uint32_t*>(nxt),
                reinterpret_cast<uint32_t*>(cur));
            if (ur != 1) {
                if (ur < 0)
                    MarkPluginBrokenLocked(st, fx); // timeout — не выгружать
                else
                    DropPluginLocked(st, fx); // AV/битый — выгрузить
                g_cpuFallback.store(true, std::memory_order_relaxed);
                return FreiResult::kNeedCpu;
            }
            // Выход плагина (cur) → BGRX в nxt; nxt становится текущим.
            FromPluginLayout(lp, cur, nxt, pixels);
            std::swap(cur, nxt);
        }
        if (fx == kFxScanlines) {
            // Контраст сканлайнов сверх фиксированных ×0.5 плагина.
            // Чётные строки плагин не трогает — правим только нечётные.
            const size_t rowBytes = (size_t)w * 4u;
            if (level <= 50) {
                // Blend-back к доснапшоту: мягкие линии на малых уровнях.
                for (uint32_t y = 1; y < h; y += 2) {
                    const uint8_t* pre = st.stageC + (size_t)y * rowBytes;
                    uint8_t* out = cur + (size_t)y * rowBytes;
                    for (size_t i = 0; i < rowBytes; i += 4)
                        for (int ch = 0; ch < 3; ++ch)
                            out[i + ch] = (uint8_t)(
                                (pre[i + ch] * (50 - level) +
                                 out[i + ch] * level + 25) /
                                50);
                }
            } else {
                // Дожим темноты: extra 100→30 (итог ×0.5×0.3=×0.15).
                const int extra = 100 - (70 * (level - 50) + 25) / 50;
                for (uint32_t y = 1; y < h; y += 2) {
                    uint8_t* out = cur + (size_t)y * rowBytes;
                    for (size_t i = 0; i < rowBytes; i += 4)
                        for (int ch = 0; ch < 3; ++ch)
                            out[i + ch] =
                                (uint8_t)((out[i + ch] * extra + 50) / 100);
                }
            }
        }
    }
    std::memcpy(bgrx, cur, bytes);
    return FreiResult::kApplied;
}

bool TakeCpuFallbackFlag() {
    return g_cpuFallback.exchange(false, std::memory_order_relaxed);
}

void SetSearchDir(const std::wstring& dir) {
    State& st = FreiState();
    std::unique_lock<std::mutex> lock(st.mutex);
    if (st.searchOverride != dir) {
        st.searchOverride = dir;
        // Смена каталога: сбросить всё загруженное (тесты требуют чистоты).
        for (int fx = 0; fx < kFxCount; ++fx) {
            LoadedPlugin& lp = st.plugins[fx];
            for (auto it = st.instances.begin(); it != st.instances.end();) {
                if (it->first.fx == fx) {
                    DestructCtx c{&lp, it->second};
                    if (lp.dll) GuardedCall(DestructThunk, &c);
                    it = st.instances.erase(it);
                } else {
                    ++it;
                }
            }
            if (lp.dll) {
                if (lp.inited) {
                    DeinitCtx dc{lp.deinit};
                    GuardedCall(DeinitThunk, &dc);
                }
                FreeLibrary(lp.dll);
            }
            lp = LoadedPlugin{};
            st.pluginsProbed[fx] = false;
        }
    }
}

void ShutdownFrei() {
    State& st = FreiState();
    std::unique_lock<std::mutex> lock(st.mutex);
    for (int fx = 0; fx < kFxCount; ++fx) {
        LoadedPlugin& lp = st.plugins[fx];
        for (auto it = st.instances.begin(); it != st.instances.end();) {
            if (it->first.fx == fx) {
                DestructCtx c{&lp, it->second};
                if (lp.dll) GuardedCall(DestructThunk, &c);
                it = st.instances.erase(it);
            } else {
                ++it;
            }
        }
        if (lp.dll) {
            if (lp.inited) {
                DeinitCtx dc{lp.deinit};
                GuardedCall(DeinitThunk, &dc);
            }
            FreeLibrary(lp.dll);
        }
        lp = LoadedPlugin{};
        st.pluginsProbed[fx] = false;
    }
    if (st.stageA) _aligned_free(st.stageA);
    if (st.stageB) _aligned_free(st.stageB);
    if (st.stageC) _aligned_free(st.stageC);
    st.stageA = st.stageB = st.stageC = nullptr;
    st.stageBytes = 0;
}

} // namespace vcam::effects::frei
