// Исполнитель эффектов хоста на GPUPixel (static lib /MT, собрана из исходников
// ef552bf8; детали — third_party/gpupixel/NOTICE.txt, vcam-effects-gpu.memory.md).
//
// Что и как (итоги аудита исходников GPUPixel, vcam-effects-gpu.memory.md):
// - GL-контекст создавать НЕ надо: GPUPixelContext внутри DLL сам поднимает
//   скрытое окно GLFW 1x1 + glad; все GL-вызовы — на внутреннем потоке через
//   SyncRunWithContext (promise/future, синхронно для вызывающего).
// - Зеркало — RotationMode::FlipHorizontal на SourceRawData (texcoords).
// - Ч/Б — GrayscaleFilter (Rec.709 luma, alpha сохраняется).
// - На Windows ProcessData(...,BGRA) — NO-OP (glTexImage2D только для iOS/Mac),
//   поэтому вход только RGBA: наш BGRX подаём как RGBA + компенсация обменом
//   B<->R (upload синхронный — in-place swap до/после; readback копируем
//   со swap + A=255). Зеркало — чистый passthrough (прямое копирование + A=255,
//   иначе R/B поменяются местами).
// - Цепочка как в demo (source->AddSink(...)->AddSink(sink_raw), затем
//   ProcessData + SinkRawData::GetRgbaBuffer). Два постоянных пайпа
//   (plain для mirror-only, gray для Ч/Б) — без перекоммутации на лету.
//
// Граница кода: вызываем только Create/AddSink/SetRotation/
// ProcessData/GetRgbaBuffer/GetWidth/GetHeight. Всё слинковано статически
// (/MT везде) — CRT общий, вопросов о кучах нет. Сырые буферы копируем сами;
// std::string/контейнеры через границу TU не передаём.
//
// Потоковая модель (факты, доказанные зондами в %TEMP% на NVIDIA RTX 3050):
// Filter-инициализация с ГЛАВНОГО потока падает с AV в nvoglv64, с фонового —
// работает; gray-Create с main при поднятом на main собственном compat
// контексте — тоже работает. Поэтому ВСЕ вызовы библиотеки — только с фоновых
// нитей (свой GpuThread + прогрев сборки на throwaway-нити), а на GpuThread
// дополнительно держим собственный скрытый compat GL-контекст текущим
// (страховка от инлайн-выполнения без контекста). Поток вызывающего лишь
// ждёт future. Любой сбой — false, кадр цел.
//
// Самолечение (факт: первая сборка в процессе бывает «успешно-битой» —
// пайпы создаются, но рендерят белый шум): выход каждого кадра проверяется
// ДО копирования в m.buf; подпись порчи (белый/нулевой выход при живом входе)
// ведёт на release + пересборку + повтор в рамках того же вызова; повторная
// порча — липкий disabled. Белый кадр в эфир не уходит никогда.
//
// Синхронизация: pipeMutex охраняет пайпы и warmedUp; stateMutex — короткие
// секции для disabled/failStreak/нити/очереди. Порядок: pipe->state.

#include "GpuEffects.h"

#include "FreiEffects.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <excpt.h>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

#include <windows.h>

#include <GL/gl.h>

#include "gpupixel/filter/grayscale_filter.h"
#include "gpupixel/sink/sink_raw_data.h"
#include "gpupixel/source/source_raw_data.h"

#pragma comment(lib, "opengl32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")

namespace vcam::effects {
namespace {

// После стольких подряд обычных неудач — липкий fail-open.
constexpr int kFailStreakDisable = 30;

struct Pipe {
    std::shared_ptr<gpupixel::SourceRawData> source;
    std::shared_ptr<gpupixel::Filter> gray; // пуст для plain-пайпа
    std::shared_ptr<gpupixel::SinkRawData> sink;
    bool built = false;
};

// SEH-барьер: внутри __try-функции только POD (указатель контекста),
// вся C++-работа — в обычных thunk-функциях; AV при битом GL превращается
// в false (fail-open).
using Thunk = void (*)(void*);

bool GuardedCall(Thunk fn, void* ctx) {
    __try {
        fn(ctx);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct BuildCtx {
    Pipe* plain;
    Pipe* gray;
};

void BuildThunk(void* p) {
    auto* ctx = static_cast<BuildCtx*>(p);
    auto s1 = gpupixel::SourceRawData::Create();
    auto k1 = gpupixel::SinkRawData::Create();
    auto s2 = gpupixel::SourceRawData::Create();
    auto g = gpupixel::GrayscaleFilter::Create();
    auto k2 = gpupixel::SinkRawData::Create();
    if (!s1 || !k1 || !s2 || !g || !k2) return; // GL не поднялся — built=false
    s1->AddSink(k1);
    s2->AddSink(g);
    g->AddSink(k2);
    ctx->plain->source = s1;
    ctx->plain->sink = k1;
    ctx->gray->source = s2;
    ctx->gray->gray = g;
    ctx->gray->sink = k2;
    ctx->plain->built = true;
    ctx->gray->built = true;
}

struct ProcessCtx {
    gpupixel::SourceRawData* source;
    gpupixel::SinkRawData* sink;
    gpupixel::RotationMode rotation;
    const uint8_t* data;
    int width;
    int height;
    int stride;
    const uint8_t* out;
    int outWidth;
    int outHeight;
};

void ProcessThunk(void* p) {
    auto* ctx = static_cast<ProcessCtx*>(p);
    ctx->source->SetRotation(ctx->rotation);
    ctx->source->ProcessData(ctx->data, ctx->width, ctx->height, ctx->stride,
                             gpupixel::GPUPIXEL_FRAME_TYPE_RGBA);
    ctx->out = ctx->sink->GetRgbaBuffer();
    ctx->outWidth = ctx->sink->GetWidth();
    ctx->outHeight = ctx->sink->GetHeight();
}

struct ReleaseCtx {
    Pipe* plain;
    Pipe* gray;
};

void ReleaseThunk(void* p) {
    auto* ctx = static_cast<ReleaseCtx*>(p);
    ctx->plain->source.reset();
    ctx->plain->gray.reset();
    ctx->plain->sink.reset();
    ctx->plain->built = false;
    ctx->gray->source.reset();
    ctx->gray->gray.reset();
    ctx->gray->sink.reset();
    ctx->gray->built = false;
}

// Обмен R<->B in-place (BGRX <-> RGBA-порядок).
void SwapRBInPlace(uint8_t* bgrx, size_t pixels) {
    for (size_t i = 0; i < pixels; ++i) {
        uint8_t* p = bgrx + i * 4u;
        const uint8_t t = p[0];
        p[0] = p[2];
        p[2] = t;
    }
}

// Анализ кадра за один проход: есть ли ненулевой RGB / не-белый / белый.
struct ScanInfo {
    bool anyNonZero = false;
    bool anyNonWhite = false; // хотя бы один пиксель не (255,255,255)
    bool allWhite = true;     // все пиксели ровно белые (пустой кадр — false)
};

ScanInfo ScanFrame(const uint8_t* px, size_t pixels) {
    ScanInfo si;
    if (pixels == 0) {
        si.allWhite = false;
        return si;
    }
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* p = px + i * 4u;
        if (p[0] | p[1] | p[2]) si.anyNonZero = true;
        if (p[0] != 255 || p[1] != 255 || p[2] != 255) {
            si.anyNonWhite = true;
            si.allWhite = false;
        }
    }
    return si;
}

// RGBA (библиотека) -> BGRX (наш кадр), A/X = 255. swappedUpload=true —
// вход был B↔R-переставлен (grayscale-путь), возвращаем обменом;
// false — passthrough (прямое копирование).
void CopyRgbaToBgrx(const uint8_t* rgba, uint8_t* bgrx, size_t pixels,
                    bool swappedUpload) {
    if (!swappedUpload) {
        for (size_t i = 0; i < pixels; ++i) {
            const uint8_t* s = rgba + i * 4u;
            uint8_t* d = bgrx + i * 4u;
            d[0] = s[0];
            d[1] = s[1];
            d[2] = s[2];
            d[3] = 255;
        }
        return;
    }
    for (size_t i = 0; i < pixels; ++i) {
        const uint8_t* s = rgba + i * 4u;
        uint8_t* d = bgrx + i * 4u;
        d[0] = s[2];
        d[1] = s[1];
        d[2] = s[0];
        d[3] = 255;
    }
}

struct OwnGl {
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
};

LRESULT __stdcall FxWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return DefWindowProcW(h, m, w, l);
}

// Скрытый compat GL-контекст, текущий на вызывающей нити. Неудача не фатальна.
bool CreateOwnGl(OwnGl& gl) {
    static const wchar_t kCls[] = L"VCamFxHiddenGL";
    static std::mutex regMutex;
    static bool registered = false;
    {
        std::unique_lock<std::mutex> lock(regMutex);
        if (!registered) {
            WNDCLASSW wc{};
            wc.style = CS_OWNDC;
            wc.lpfnWndProc = FxWndProc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = kCls;
            if (!RegisterClassW(&wc)) return false;
            registered = true;
        }
    }
    HWND wnd = CreateWindowExW(0, kCls, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (!wnd) return false;
    HDC dc = GetDC(wnd);
    if (!dc) {
        DestroyWindow(wnd);
        return false;
    }
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pfd.iLayerType = PFD_MAIN_PLANE;
    const int pf = ChoosePixelFormat(dc, &pfd);
    bool ok = (pf != 0) && (SetPixelFormat(dc, pf, &pfd) != FALSE);
    HGLRC rc = nullptr;
    if (ok) {
        rc = wglCreateContext(dc);
        ok = (rc != nullptr) && (wglMakeCurrent(dc, rc) != FALSE);
    }
    if (!ok) {
        if (rc) wglDeleteContext(rc);
        ReleaseDC(wnd, dc);
        DestroyWindow(wnd);
        return false;
    }
    gl.wnd = wnd;
    gl.dc = dc;
    gl.rc = rc;
    return true;
}

void DestroyOwnGl(OwnGl& gl) {
    if (gl.rc) {
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(gl.rc);
        gl.rc = nullptr;
    }
    if (gl.dc && gl.wnd) {
        ReleaseDC(gl.wnd, gl.dc);
        gl.dc = nullptr;
    }
    if (gl.wnd) {
        DestroyWindow(gl.wnd);
        gl.wnd = nullptr;
    }
}

bool FxDbg() {
    static const bool on = (std::getenv("VCAM_GPUFX_DEBUG") != nullptr);
    return on;
}

struct State {
    std::mutex pipeMutex;  // пайпы + warmedUp
    std::mutex stateMutex; // disabled/failStreak/нить/очередь
    std::condition_variable cv;
    std::thread worker;
    bool workerRunning = false;
    bool stopRequested = false;
    struct Job {
        Thunk fn = nullptr;
        void* ctx = nullptr;
        // shared_ptr (а не голый &promise со стека вызывающего): при таймауте
        // К3 вызывающий уходит, а worker позже всё равно сделает set_value —
        // по висячему указателю это был бы UAF. Копия в очереди держит
        // promise живым; set_value в пустоту безопасен.
        std::shared_ptr<std::promise<bool>> done;
    };
    std::queue<Job> jobs;
    Pipe plain; // source -> sink (только зеркало)
    Pipe gray;  // source -> GrayscaleFilter -> sink (Ч/Б [+ зеркало])
    bool warmedUp = false; // прогрев сборки был (одноразовая нить)
    bool disabled = false;
    int failStreak = 0;
    OwnGl ownGl;
    bool ownGlOk = false;

    ~State() {
        // Backstop на случай забытого ShutdownEffects: DLL уже не дёргаем,
        // только останавливаем свою нить (join без обращений к библиотеке).
        std::unique_lock<std::mutex> lock(stateMutex);
        stopRequested = true;
        lock.unlock();
        cv.notify_all();
        if (worker.joinable()) worker.join();
        DestroyOwnGl(ownGl);
    }
};

State& FxState() {
    static State s;
    return s;
}

void GpuWorkerLoop(State* st) {
    // Свой контекст — первым делом на этой нити, текущим навсегда.
    OwnGl gl{};
    const bool glOk = CreateOwnGl(gl);
    {
        std::unique_lock<std::mutex> lock(st->stateMutex);
        st->ownGl = gl;
        st->ownGlOk = glOk;
    }
    if (FxDbg()) {
        std::fprintf(stderr, "[fx] own GL: %d\n", (int)glOk);
    }
    for (;;) {
        State::Job job;
        {
            std::unique_lock<std::mutex> lock(st->stateMutex);
            st->cv.wait(lock, [&] { return st->stopRequested || !st->jobs.empty(); });
            if (st->stopRequested && st->jobs.empty()) {
                wglMakeCurrent(nullptr, nullptr); // отцепить свой контекст
                return;
            }
            job = st->jobs.front();
            st->jobs.pop();
        }
        bool r = false;
        try {
            r = GuardedCall(job.fn, job.ctx);
        } catch (...) {
            r = false;
        }
        try {
            job.done->set_value(r);
        } catch (...) {
        }
    }
}

// Выполнить thunk на GpuThread синхронно. false = нить/вызов сломаны.
// К3: голый fu.get() без таймаута вешал worker host-кадра навсегда при
// зависшем GL — ждём ~500 мс, дальше fail-open (false, кадр без эффектов).
// stateMutex держится только на время постановки (во время ожидания свободен).
bool RunOnGpuThread(State& st, Thunk fn, void* ctx) {
    std::unique_lock<std::mutex> lock(st.stateMutex);
    if (!st.workerRunning) {
        st.stopRequested = false; // рестарт после ShutdownEffects
        try {
            st.worker = std::thread(GpuWorkerLoop, &st);
            st.workerRunning = true;
        } catch (...) {
            return false;
        }
    }
    auto pr = std::make_shared<std::promise<bool>>();
    std::future<bool> fu = pr->get_future();
    st.jobs.push(State::Job{fn, ctx, pr});
    lock.unlock();
    st.cv.notify_one();
    try {
        if (fu.wait_for(std::chrono::milliseconds(500)) != std::future_status::ready)
            return false; // timeout: очередь держит свою копию pr — UAF нет
        return fu.get();
    } catch (...) {
        return false;
    }
}

void StopGpuThread(State& st) {
    std::unique_lock<std::mutex> lock(st.stateMutex);
    st.stopRequested = true;
    lock.unlock();
    st.cv.notify_all();
    if (st.worker.joinable()) st.worker.join();
    lock.lock();
    st.workerRunning = false;
    DestroyOwnGl(st.ownGl);
    st.ownGlOk = false;
}

void NoteFail(State& st, bool disableNow) {
    std::unique_lock<std::mutex> lock(st.stateMutex);
    if (disableNow || ++st.failStreak >= kFailStreakDisable) st.disabled = true;
}

void NoteOk(State& st) {
    std::unique_lock<std::mutex> lock(st.stateMutex);
    st.failStreak = 0;
}

bool IsDisabled(State& st) {
    std::unique_lock<std::mutex> lock(st.stateMutex);
    return st.disabled;
}

// pipeMutex вызывающего уже взят. Только сборка (без валидации — её делает
// кадр: подпись порчи видна лишь на реальном выходе).
bool EnsureBuiltLocked(State& st) {
    if (st.plain.built && st.gray.built) return true;
    if (!st.warmedUp) {
        // Прогрев сборки на одноразовой нити: первый Filter-touch в процессе
        // с main падает с AV; пусть рискует throwaway-нить (SEH ловит).
        st.warmedUp = true;
        BuildCtx bctx{&st.plain, &st.gray};
        try {
            std::promise<bool> pr;
            std::future<bool> fu = pr.get_future();
            std::thread warm([&] {
                bool r = false;
                try {
                    r = GuardedCall(BuildThunk, &bctx);
                } catch (...) {
                    r = false;
                }
                try {
                    pr.set_value(r);
                } catch (...) {
                }
            });
            warm.join();
            (void)fu.get();
        } catch (...) {
        }
        // Итог прогрева не важен: чистим частичное, собираем на GpuThread.
        ReleaseCtx rctx{&st.plain, &st.gray};
        RunOnGpuThread(st, ReleaseThunk, &rctx);
    }
    BuildCtx bctx{&st.plain, &st.gray};
    return RunOnGpuThread(st, BuildThunk, &bctx) && st.plain.built && st.gray.built;
}

} // namespace

// ---- CPU-аналог помех (без внешних зависимостей; работает и без GPU) ----
// Кадр-счётчик для анимации: сид PRNG каждого кадра = splitmix64(frame^salt),
// поэтому шум и трекинг меняются от кадра к кадру, но детерминированы внутри.
std::atomic<uint64_t> g_analogFrame{0};

uint64_t SplitMix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ULL;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ULL;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBULL;
    return x ^ (x >> 31);
}

uint32_t XorShift32(uint32_t& s) {
    s ^= s << 13;
    s ^= s >> 17;
    s ^= s << 5;
    return s;
}

uint8_t ClampU8(int v) { return (uint8_t)(v < 0 ? 0 : v > 255 ? 255 : v); }

// Белый/RGB-шум: амплитуда ±(level% от ±60), т.е. amp=(level*60+50)/100
// (100 → ±60 явно видно в превью 720p, 50 → ±30, 0 → no-op). Три xorshift
// на пиксель (по одному на канал), каналы независимы. "NOISE"-salt даёт
// независимый поток от трекинга.
void CpuNoise(uint8_t* px, size_t pixels, uint64_t frame, int level) {
    const int amp = (level * 60 + 50) / 100;
    if (amp <= 0) return;
    const int range = amp * 2 + 1;
    uint32_t s = (uint32_t)SplitMix64(frame ^ 0x4E4F495345ULL);
    if (s == 0) s = 0x243F6A88u;
    for (size_t i = 0; i < pixels; ++i, px += 4) {
        px[0] = ClampU8((int)px[0] + (int)(XorShift32(s) % (uint32_t)range) - amp);
        px[1] = ClampU8((int)px[1] + (int)(XorShift32(s) % (uint32_t)range) - amp);
        px[2] = ClampU8((int)px[2] + (int)(XorShift32(s) % (uint32_t)range) - amp);
    }
}

// Scanlines: глубина затемнения нечётных строк num/256,
// num=256−(166*level+50)/100 (100 → ×90/256 ≈ 0.35 явно видно — CRT-маска,
// 50 → ×173/256 ≈ 0.68, 0 → ×256/256, т.е. без изменений).
void CpuScanlines(uint8_t* px, uint32_t w, uint32_t h, int level) {
    if (level <= 0) return;
    const uint32_t num = (uint32_t)(256 - (166 * level + 50) / 100);
    if (num >= 256) return;
    const size_t rowBytes = (size_t)w * 4u;
    for (uint32_t y = 1; y < h; y += 2) {
        uint8_t* row = px + (size_t)y * rowBytes;
        for (size_t i = 0; i < rowBytes; i += 4) {
            row[i] = (uint8_t)((row[i] * num) >> 8);
            row[i + 1] = (uint8_t)((row[i + 1] * num) >> 8);
            row[i + 2] = (uint8_t)((row[i + 2] * num) >> 8);
        }
    }
}

// Хроматическая аберрация: R берём правее на dx, B — левее (построчный temp).
// dx=(dx100*level+50)/100, dx100=6/12/24 по ширине (720p → 12px в каждую
// сторону — явно видно, 0 → no-op).
void CpuRgbSplit(uint8_t* px, uint32_t w, uint32_t h, int level) {
    if (level <= 0) return;
    const int dx100 = (w >= 2560) ? 24 : (w >= 1280) ? 12 : 6;
    const int dx = (dx100 * level + 50) / 100;
    if (dx <= 0 || (uint32_t)dx >= w) return;
    const size_t rowBytes = (size_t)w * 4u;
    static thread_local std::vector<uint8_t> tmp; // переиспользуем; throw -> catch в ApplyEffects
    tmp.resize(rowBytes);
    for (uint32_t y = 0; y < h; ++y) {
        uint8_t* row = px + (size_t)y * rowBytes;
        std::memcpy(tmp.data(), row, rowBytes);
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t xr = x + (uint32_t)dx;
            if (xr >= w) xr = w - 1;
            const uint32_t xb = (x >= (uint32_t)dx) ? x - (uint32_t)dx : 0;
            row[x * 4u + 2] = tmp[xr * 4u + 2]; // R справа
            row[x * 4u + 0] = tmp[xb * 4u + 0]; // B слева
        }
    }
}

// Трекинг-глитч: полосы/кадр со случайным горизонтальным сдвигом (wrap)
// + белая 2px-строка сверху полосы (head-switching). Позиции/сдвиги —
// от frame-сида, поэтому полосы движутся от кадра к кадру.
// Масштаб от level (100 → 4–8 полос, h=8..h/8, сдвиг ±(8..w/10) —
// явно видно; 0 → нет полос): число полос, высота и сдвиг умножаются на
// level% (минимумы 1 полоса / 2px высота / 1px сдвиг при level>0).
// PRNG-последовательность та же, что без уровней, поэтому слабый уровень —
// префикс полного прогона (монотонность: больше level → больше изменений).
void CpuTracking(uint8_t* px, uint32_t w, uint32_t h, uint64_t frame,
                 int level) {
    if (level <= 0) return;
    if (w < 16 || h < 16) return;
    const size_t rowBytes = (size_t)w * 4u;
    static thread_local std::vector<uint8_t> snap; // переиспользуем: без alloc на кадр
    static thread_local std::vector<uint8_t> tmp;
    snap.resize((size_t)h * rowBytes);
    std::memcpy(snap.data(), px, snap.size());
    tmp.resize(rowBytes);
    uint32_t s = (uint32_t)SplitMix64(frame ^ 0x545241434BULL);
    if (s == 0) s = 0x452821E7u;
    const int bandsBase = 4 + (int)(XorShift32(s) % 5u);
    int bands = (bandsBase * level + 50) / 100;
    if (bands < 1) bands = 1;
    if (bands > bandsBase) bands = bandsBase;
    const uint32_t maxBandH = h / 8u >= 8u ? h / 8u : 8u;
    const uint32_t maxOff = w / 10u >= 8u ? w / 10u : 8u;
    for (int b = 0; b < bands; ++b) {
        const uint32_t bandH100 = 8u + XorShift32(s) % maxBandH;
        uint32_t bandH = (bandH100 * (uint32_t)level + 50u) / 100u;
        if (bandH < 2u) bandH = 2u;
        const uint32_t y0 = XorShift32(s) % h;
        uint32_t y1 = y0 + bandH;
        if (y1 > h) y1 = h;
        int off100 = 8 + (int)(XorShift32(s) % maxOff);
        int off = (off100 * level + 50) / 100;
        if (off < 1) off = 1;
        if (XorShift32(s) & 1u) off = -off;
        for (uint32_t y = y0; y < y1; ++y) {
            const uint8_t* src = snap.data() + (size_t)y * rowBytes;
            uint8_t* dst = px + (size_t)y * rowBytes;
            std::memcpy(tmp.data(), src, rowBytes);
            for (uint32_t x = 0; x < w; ++x) {
                int sx = ((int)x - off) % (int)w;
                if (sx < 0) sx += (int)w;
                const uint8_t* p = tmp.data() + (size_t)sx * 4u;
                uint8_t* d = dst + (size_t)x * 4u;
                d[0] = p[0];
                d[1] = p[1];
                d[2] = p[2];
                d[3] = 255; // М7: BGRX X=255 как GPU-путь (CopyRgbaToBgrx)
                            // и frei-путь (FromPluginLayout); иначе alpha
                            // остаётся от старого пикселя назначения
            }
        }
        // Белая строка head-switching поверх полосы (до 2px, если влезли).
        for (uint32_t y = y0; y < y0 + 2 && y < y1; ++y) {
            uint8_t* dst = px + (size_t)y * rowBytes;
            for (size_t i = 0; i < rowBytes; i += 4) {
                dst[i] = dst[i + 1] = dst[i + 2] = dst[i + 3] = 255; // М7: и X тоже
            }
        }
    }
}

// Пайпы + shared_ptr-копии под pipeMutex; сам Process — на GpuThread без
// мьютексов (копии держат цепочку живой даже при чужой пересборке).
// GPU-стадия mirror/grayscale: false — кадр не тронут (swap откачен).
bool RunGpuEffects(State& st, uint8_t* bgrx, size_t pixels,
                          uint32_t w, uint32_t h, int stride,
                          bool mirror, bool grayscale) {
    bool gpuOk = false;
    bool swapped = false;
    std::shared_ptr<gpupixel::SourceRawData> src;
    std::shared_ptr<gpupixel::SinkRawData> snk;
    ScanInfo inputScan{};
    {
        std::unique_lock<std::mutex> plock(st.pipeMutex);
        if (!EnsureBuiltLocked(st)) {
            NoteFail(st, false);
            return false;
        }
        Pipe& pipe = grayscale ? st.gray : st.plain;
        src = pipe.source;
        snk = pipe.sink;
        if (!src || !snk) {
            NoteFail(st, false);
            return false;
        }
        if (grayscale) {
            SwapRBInPlace(bgrx, pixels);
            swapped = true;
        }
        inputScan = ScanFrame(bgrx, pixels);
    }

    // Один повтор при подписи порчи (release + пересборка + тот же кадр).
    for (int attempt = 0; attempt < 2; ++attempt) {
        bool frameOk = false;
        bool corrupt = false;
        try {
            ProcessCtx pctx{};
            pctx.source = src.get();
            pctx.sink = snk.get();
            pctx.rotation = mirror ? gpupixel::FlipHorizontal : gpupixel::NoRotation;
            pctx.data = bgrx;
            pctx.width = (int)w;
            pctx.height = (int)h;
            pctx.stride = stride;
            if (RunOnGpuThread(st, ProcessThunk, &pctx) && pctx.out &&
                pctx.outWidth == (int)w && pctx.outHeight == (int)h) {
                const ScanInfo outScan = ScanFrame(pctx.out, pixels);
                if (!outScan.anyNonZero && inputScan.anyNonZero) {
                    // Нулевой выход при живом входе: GL ничего не отрендерил
                    // (буфер sink изначально memset 0).
                    corrupt = true;
                } else if (outScan.allWhite && inputScan.anyNonWhite) {
                    // Белый выход при не-белом входе: порча пайпа
                    // («успешно-битая» первая сборка). В эфир не идёт.
                    corrupt = true;
                } else {
                    CopyRgbaToBgrx(pctx.out, bgrx, pixels, grayscale);
                    frameOk = true;
                }
            }
        } catch (...) {
        }
        if (frameOk) {
            swapped = false; // CopyRgbaToBgrx уже вернул порядок BGRX
            gpuOk = true;
            NoteOk(st);
            break;
        }
        if (!corrupt) {
            // Обычная неудача без подписи порчи (вызов упал): откат swap,
            // дальше — липкий счётчик.
            if (swapped) SwapRBInPlace(bgrx, pixels);
            NoteFail(st, false);
            return false;
        }
        // Порча: откат swap (повтор увидит исходный вход), release +
        // пересборка + повтор тем же кадром.
        if (FxDbg()) {
            std::fprintf(stderr, "[fx] corrupt output, rebuild+retry (attempt %d)\n", attempt);
        }
        if (swapped) SwapRBInPlace(bgrx, pixels);
        {
            std::unique_lock<std::mutex> plock(st.pipeMutex);
            ReleaseCtx rctx{&st.plain, &st.gray};
            RunOnGpuThread(st, ReleaseThunk, &rctx);
            st.warmedUp = true; // прогрев уже был, не повторяем
            if (!EnsureBuiltLocked(st)) {
                NoteFail(st, true); // среда битая — сразу disabled
                return false;
            }
            Pipe& pipe = grayscale ? st.gray : st.plain;
            src = pipe.source;
            snk = pipe.sink;
            if (!src || !snk) {
                NoteFail(st, true);
                return false;
            }
            if (grayscale) {
                SwapRBInPlace(bgrx, pixels);
                swapped = true;
            } else {
                swapped = false;
            }
            inputScan = ScanFrame(bgrx, pixels);
        }
    } // for attempt
    if (!gpuOk) {
        // Повтор тоже битый — липкий disabled, кадр не тронут (swap откачен).
        // Сюда же попадаем при обычном GPU-фейле: CPU-аналог пропускаем
        // (атомарно всё-или-ничего, один one-shot лог у вызывающего).
        if (swapped) SwapRBInPlace(bgrx, pixels);
        NoteFail(st, true);
        return false;
    }
    return true;
}

bool ApplyEffects(uint8_t* bgrx, int stride, uint32_t w, uint32_t h,
                  const FxFlags& fx) {
    if (!bgrx || w == 0 || h == 0) return true;
    const bool mirror = fx.mirror;
    const bool grayscale = fx.grayscale;
    const bool needGpu = mirror || grayscale;
    bool noise = fx.noise;
    bool scanlines = fx.scanlines;
    bool rgbSplit = fx.rgbSplit;
    bool tracking = fx.tracking;
    if (fx.vhs) noise = scanlines = rgbSplit = tracking = true;
    // Уровни — индивидуальные (VHS отдельного уровня не имеет, берёт те же).
    // Уровень 0 при включённом тоггле ≈ эффект выключен (функции — no-op).
    const int noiseLevel = fx.noiseLevel;
    const int scanlinesLevel = fx.scanlinesLevel;
    const int rgbSplitLevel = fx.rgbSplitLevel;
    const int trackingLevel = fx.trackingLevel;
    const bool wantCpu = (noise && noiseLevel > 0) ||
                         (scanlines && scanlinesLevel > 0) ||
                         (rgbSplit && rgbSplitLevel > 0) ||
                         (tracking && trackingLevel > 0);
    if (!needGpu && !wantCpu) return true;
    if (stride != (int)(w * 4u)) return false; // только плотная упаковка
    const size_t pixels = (size_t)w * h;
    if (pixels == 0 || pixels > (size_t)16384 * 16384) return false;

    State& st = FxState();
    if (needGpu && IsDisabled(st)) return false;

    // GPU-кусок — только при mirror/grayscale; чистый аналог работает и без
    // GPU (Session 0), GL тогда не трогаем вообще.
    if (needGpu && !RunGpuEffects(st, bgrx, pixels, w, h, stride, mirror, grayscale))
        return false;

    // Помехи: backend cpu (штатный путь, бит-в-бит как раньше) или frei0r
    // (цепочка плагинов; недоступны → kNeedCpu → тот же CPU-путь + флаг
    // для one-shot лога хоста). VHS на обоих — связка тех же четырёх.
    // Единственный сбой CPU-пути — OOM temp-буфера (кадр не тронут —
    // аллокации все ДО модификации); сами циклы не бросают.
    // М8 (designed, не менять функционально): g_analogFrame стоит при
    // выключенном аналоге — счётчик крутится только внутри if (wantCpu),
    // т.е. анимация шума/трекинга ставится на паузу вместо дрейфа фазы.
    // При повторном включении помехи продолжаются с того же кадра.
    if (wantCpu) {
        const uint64_t frame =
            g_analogFrame.fetch_add(1, std::memory_order_relaxed);
        auto runCpu = [&]() {
            try {
                if (rgbSplit && rgbSplitLevel > 0) CpuRgbSplit(bgrx, w, h, rgbSplitLevel);
                if (tracking && trackingLevel > 0)
                    CpuTracking(bgrx, w, h, frame, trackingLevel);
                if (noise && noiseLevel > 0) CpuNoise(bgrx, pixels, frame, noiseLevel);
                if (scanlines && scanlinesLevel > 0)
                    CpuScanlines(bgrx, w, h, scanlinesLevel);
            } catch (...) {
                return false;
            }
            return true;
        };
        if (fx.backend == L"frei0r") {
            frei::AnalogRequest fr{};
            fr.noise = noise;
            fr.scanlines = scanlines;
            fr.rgbSplit = rgbSplit;
            fr.tracking = tracking;
            fr.noiseLevel = noiseLevel;
            fr.scanlinesLevel = scanlinesLevel;
            fr.rgbSplitLevel = rgbSplitLevel;
            fr.trackingLevel = trackingLevel;
            fr.timeSec = (double)frame / 30.0;
            const frei::FreiResult r = frei::ApplyAnalog(bgrx, w, h, fr);
            if (r == frei::FreiResult::kApplied) return true;
            if (r == frei::FreiResult::kFailed) return false;
            if (!runCpu()) return false; // kNeedCpu → fail-open на CPU
        } else {
            if (!runCpu()) return false;
        }
    }
    return true;
}

bool TakeFreiFallbackFlag() { return frei::TakeCpuFallbackFlag(); }

void ShutdownEffects() {
    State& st = FxState();
    std::unique_lock<std::mutex> plock(st.pipeMutex);
    ReleaseCtx ctx{&st.plain, &st.gray};
    RunOnGpuThread(st, ReleaseThunk, &ctx);
    StopGpuThread(st);
    std::unique_lock<std::mutex> slock(st.stateMutex);
    st.failStreak = 0;
    // warmedUp/disabled не сбрасываем: warmedUp одноразовый за процесс,
    // disabled — липкий (среда не чинится перезапуском пайпа).
    frei::ShutdownFrei();
}

} // namespace vcam::effects
