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
// - Маппинг level (монотонно, level 0 = пропуск плагина = точный no-op):
//   noise:     v = L/100 (100 → σ~127, явно видно);
//   scanlines: params нет, фикс ×0.5 нечётных (L>0 применить);
//   rgbsplit:  vert 0.5 (без вертикали — паритет с CPU), horiz 0.5+(L/100)*0.075
//     → dx = L/100*w/106.7 (12px при 1280 — как CPU-максимум);
//   tracking:  freq L/100, block 0.02+0.10*L/100, shift 0.02+0.08*L/100
//     (100 → сдвиг до 10% ширины), color L/100.
// Порядок цепочки — как CPU: rgbsplit → tracking(glitch0r) → noise → scanlines.
//
// Потокобезопасность: один мьютекс на весь вызов (заодно сериализует
// rand() внутри rgbnoise/glitch0r). AV в чужом коде ловим SEH и помечаем
// плагин битым (выгружаем) → kNeedCpu, кадр НЕ тронут (работаем на копиях,
// в bgrx пишем только после успеха всей цепочки — атомарно).
// Выравнивание: спека требует 16 байт — stage-буферы через _aligned_malloc.

#include "FreiEffects.h"

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
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

    if (st.stageBytes < bytes || !st.stageA || !st.stageB) {
        if (st.stageA) _aligned_free(st.stageA);
        if (st.stageB) _aligned_free(st.stageB);
        st.stageA = (uint8_t*)_aligned_malloc(bytes, 16);
        st.stageB = (uint8_t*)_aligned_malloc(bytes, 16);
        if (!st.stageA || !st.stageB) {
            if (st.stageA) _aligned_free(st.stageA);
            if (st.stageB) _aligned_free(st.stageB);
            st.stageA = st.stageB = nullptr;
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
        switch (fx) {
        case kFxNoise:
            SetDouble(lp, inst, 0, L);
            break;
        case kFxScanlines:
            break; // params нет — фикс ×0.5 нечётных
        case kFxRgbSplit:
            SetDouble(lp, inst, 0, 0.5);              // vertical: без сдвига
            SetDouble(lp, inst, 1, 0.5 + L * 0.075);  // horiz: dx=12px@1280
            break;
        case kFxTracking:
            SetDouble(lp, inst, 0, L);                 // freq
            SetDouble(lp, inst, 1, 0.02 + 0.10 * L);   // block
            SetDouble(lp, inst, 2, 0.02 + 0.08 * L);   // shift (100 → 10% w)
            SetDouble(lp, inst, 3, L);                 // color
            break;
        }
        // Стадия: BGRX(cur) → layout плагина(nxt) → update(in=nxt, out=cur)
        // → BGRX(cur→nxt); nxt — текущий. В bgrx пишем только после успеха
        // всей цепочки (атомарно).
        ToPluginLayout(lp, cur, nxt, pixels);
        UpdateCtx c{&lp, inst, req.timeSec,
                    reinterpret_cast<const uint32_t*>(nxt),
                    reinterpret_cast<uint32_t*>(cur), false};
        if (!GuardedCall(UpdateThunk, &c) || !c.ok) {
            DropPluginLocked(st, fx); // AV/битый — выгрузить
            g_cpuFallback.store(true, std::memory_order_relaxed);
            return FreiResult::kNeedCpu;
        }
        // Выход плагина (cur) → BGRX в nxt; nxt становится текущим.
        FromPluginLayout(lp, cur, nxt, pixels);
        std::swap(cur, nxt);
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
    st.stageA = st.stageB = nullptr;
    st.stageBytes = 0;
}

} // namespace vcam::effects::frei
