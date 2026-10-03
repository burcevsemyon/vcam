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

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <excpt.h>
#include <future>
#include <mutex>
#include <queue>
#include <thread>

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
        std::promise<bool>* done = nullptr;
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
    std::promise<bool> pr;
    std::future<bool> fu = pr.get_future();
    st.jobs.push(State::Job{fn, ctx, &pr});
    lock.unlock();
    st.cv.notify_one();
    try {
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

bool ApplyEffects(uint8_t* bgrx, int stride, uint32_t w, uint32_t h,
                  bool mirror, bool grayscale) {
    if (!bgrx || w == 0 || h == 0) return true;
    if (!mirror && !grayscale) return true;
    if (stride != (int)(w * 4u)) return false; // только плотная упаковка
    const size_t pixels = (size_t)w * h;
    if (pixels == 0 || pixels > (size_t)16384 * 16384) return false;

    State& st = FxState();
    if (IsDisabled(st)) return false;

    // Пайпы + shared_ptr-копии под pipeMutex; сам Process — на GpuThread без
    // мьютексов (копии держат цепочку живой даже при чужой пересборке).
    std::shared_ptr<gpupixel::SourceRawData> src;
    std::shared_ptr<gpupixel::SinkRawData> snk;
    ScanInfo inputScan{};
    bool swapped = false;
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
            NoteOk(st);
            return true;
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
    }
    // Повтор тоже битый — липкий disabled, кадр не тронут (swap откачен).
    if (swapped) SwapRBInPlace(bgrx, pixels);
    NoteFail(st, true);
    return false;
}

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
}

} // namespace vcam::effects
