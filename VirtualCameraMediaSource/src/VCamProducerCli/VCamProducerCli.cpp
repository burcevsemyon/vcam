#include <windows.h>
#include <atlbase.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CameraDevices.h"
#include "CameraControls.h"
#include "ControlServer.h"
#include "FrameWriter.h"
#include "ProducerApi.h"
#include "Settings.h"
#include "SettingsWatcher.h"
#include "SharedMemoryContract.h"
#include "MappedViewOfFilePtr.h"

namespace {

constexpr wchar_t kHostMutexName[] = L"VCamVideoStreamProducer.Instance";

constexpr DWORD kFrameMs = 33;
constexpr DWORD kSwitchWindowMs = 5000; // окно hot-switch: FlushLast, пока новый источник не готов
constexpr DWORD kOpenRetryMs = 250;
constexpr DWORD kFallbackRetryMs = 1000;

ATL::CHandle g_stop;
ATL::CHandle g_dirty;
ATL::CHandle g_worker;
SettingsWatcher g_watcher;
std::wstring g_fixType; // --type: фиксирует тип на весь запуск
std::wstring g_fixPath; // --path/--device: фиксирует путь (для camera — id) на весь запуск
bool g_fixDevice = false; // --device: id камеры + camName очищается (не подмешиваем имя из settings)

// Вывод без CRT-локалей: консоль -> WriteConsoleW, файл/пайп -> UTF-8 WriteFile.
void Emit(bool err, const std::wstring& line)
{
    HANDLE h = GetStdHandle(err ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) return;
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        DWORD n = 0;
        WriteConsoleW(h, line.c_str(), (DWORD)line.size(), &n, nullptr);
        return;
    }
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(),
                                    nullptr, 0, nullptr, nullptr);
    if (bytes <= 0) return;
    std::string s((size_t)bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, line.c_str(), (int)line.size(), &s[0], bytes,
                        nullptr, nullptr);
    DWORD n = 0;
    WriteFile(h, s.data(), (DWORD)s.size(), &n, nullptr);
}

void Logv(bool err, const wchar_t* fmt, va_list ap)
{
    wchar_t buf[4096];
    vswprintf_s(buf, fmt, ap);
    std::wstring line(buf);
    line += L'\n';
    Emit(err, line);
}

void Log(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Logv(false, fmt, ap);
    va_end(ap);
}

void LogErr(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    Logv(true, fmt, ap);
    va_end(ap);
}

// Метка источника для статуса/логов: для camera — имя, если задано (короткое),
// иначе path (symlink длинный и засоряет консоль).
std::wstring TargetLabel(const SourceConfig& c)
{
    if (c.type == L"camera" && !c.camName.empty()) return c.camName;
    return c.path;
}

// 1 = мьютекс хоста занят (хост запущен), 0 = не запущен/не создан.
int HostRunning()
{
    ATL::CHandle m(OpenMutexW(SYNCHRONIZE, FALSE, kHostMutexName));
    if (!m) return 0;
    DWORD r = WaitForSingleObject(m, 0);
    int running = (r == WAIT_TIMEOUT) ? 1 : 0;
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(m);
    return running;
}

bool ReadSmallFile(const std::wstring& path, std::string& out)
{
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1000000) {
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    return ok && read == out.size();
}

// Новая схема = есть ключ-секция source/static/video/camera (значение-строка
// "video" в legacy-файле не считается ключом: после него нет ':').
bool LooksNewSchema(const std::string& raw)
{
    const char* keys[] = { "\"source\"", "\"static\"", "\"video\"", "\"camera\"" };
    for (const char* k : keys) {
        size_t p = raw.find(k);
        while (p != std::string::npos) {
            size_t q = p + strlen(k);
            while (q < raw.size() && (raw[q] == ' ' || raw[q] == '\t' ||
                                      raw[q] == '\r' || raw[q] == '\n'))
                q++;
            if (q < raw.size() && raw[q] == ':') return true;
            p = raw.find(k, p + 1);
        }
    }
    return false;
}

void PrintSectionState()
{
    // Писатель может работать в Local\ (fallback без SeCreateGlobalPrivilege) —
    // перебираем префиксы так же, как это делает SharedMemoryFrameSource.
    const wchar_t* pSep = wcschr(vcam::VCamSectionName, L'\\');
    const wchar_t* baseName = (pSep != nullptr) ? pSep + 1 : vcam::VCamSectionName;
    const wchar_t* prefixes[2] = { L"Global\\", L"Local\\" };
    wchar_t sectionName[128] = {};
    ATL::CHandle h;
    for (int i = 0; i < 2 && !h; i++) {
        swprintf_s(sectionName, L"%s%s", prefixes[i], baseName);
        h.Attach(OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName));
    }
    if (!h) {
        Log(L"writer section %s: not open (no producer holds it)", vcam::VCamSectionName);
        return;
    }
    void* raw = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(vcam::VCamSectionHeader));
    vcam::MappedViewOfFilePtr view(raw);
    if (!view) {
        Log(L"writer section %s: open (MapView failed: %lu)", vcam::VCamSectionName,
            GetLastError());
        return;
    }
    LONGLONG seq1 = view.GetAs<vcam::VCamSectionHeader>()->seq;
    Sleep(300);
    LONGLONG seq2 = view.GetAs<vcam::VCamSectionHeader>()->seq;
    if (seq2 != seq1) {
        Log(L"writer section %s: open, frames are being written (seq %lld -> %lld)",
            sectionName, seq1, seq2);
    } else {
        Log(L"writer section %s: open, no new frames in the last 300 ms (seq %lld)",
            sectionName, seq1);
    }
}

void PrintV2SectionState()
{
    // v2-зеркало писателя (dual-write фазы vcam-quality-v2): перебор префиксов
    // Global -> Local, как у читателей. Показывает версию/размер/seq.
    const wchar_t* pSep = wcschr(vcam::VCamSectionNameV2, L'\\');
    const wchar_t* baseName = (pSep != nullptr) ? pSep + 1 : vcam::VCamSectionNameV2;
    const wchar_t* prefixes[2] = { L"Global\\", L"Local\\" };
    wchar_t sectionName[128] = {};
    ATL::CHandle h;
    for (int i = 0; i < 2 && !h; i++) {
        swprintf_s(sectionName, L"%s%s", prefixes[i], baseName);
        h.Attach(OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName));
    }
    if (!h) {
        Log(L"v2 section %s: not open (v1-only mode - no producer holds it)",
            vcam::VCamSectionNameV2);
        return;
    }
    void* raw = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(vcam::VCamSectionHeader));
    vcam::MappedViewOfFilePtr view(raw);
    if (!view) {
        Log(L"v2 section %s: open (MapView failed: %lu)", vcam::VCamSectionNameV2,
            GetLastError());
        return;
    }
    auto* hdr = view.GetAs<vcam::VCamSectionHeader>();
    if (hdr->magic != vcam::VCamMagic) {
        Log(L"v2 section %s: open, BAD magic 0x%08X (version=%u)", sectionName,
            hdr->magic, hdr->version);
        return;
    }
    LONGLONG seq1 = hdr->seq;
    Sleep(300);
    LONGLONG seq2 = hdr->seq;
    if (seq2 != seq1) {
        Log(L"v2 section %s: open, ver=%u %ux%u stride=%u slots=%u, frames are being written (seq %lld -> %lld)",
            sectionName, hdr->version, hdr->width, hdr->height, hdr->stride,
            hdr->slotCount, seq1, seq2);
    } else {
        Log(L"v2 section %s: open, ver=%u %ux%u stride=%u slots=%u, no new frames in the last 300 ms (seq %lld)",
            sectionName, hdr->version, hdr->width, hdr->height, hdr->stride,
            hdr->slotCount, seq1);
    }
}

enum class Phase { Switch, Active, Fallback };

void FlushOrSleep(FrameWriter& w)
{
    if (!w.FlushLast()) Sleep(kFrameMs);
}

struct Machine {
    FrameWriter writer;
    bool writerOpen = false;
    std::wstring writerErr;
    std::unique_ptr<IFrameSource> src;
    SourceConfig target;
    bool hasTarget = false;
    std::wstring quality = L"source"; // Settings.quality; смена -> переоткрытие
    Phase phase = Phase::Switch;
    ULONGLONG switchStart = 0;
    ULONGLONG nextAttempt = 0;
    std::vector<uint8_t> buf; // кадр frameW x frameH BGRX (stride frameW*4)
    uint32_t frameW = vcam::VCamWidth;
    uint32_t frameH = vcam::VCamHeight;
    bool nativeKnown = false; // NativeSize отдавал размер (иначе 720p-путь)
};

void CloseSource(Machine& m)
{
    if (m.src) {
        m.src->Close();
        m.src.reset();
    }
}

// Буфер под WxH BGRX (stride W*4). false = мусор размера / сверх cap / OOM.
bool EnsureFrameBuf(Machine& m, uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0 || w > vcam::VCamNativeCapW || h > vcam::VCamNativeCapH)
        return false;
    uint64_t need = (uint64_t)h * w * 4;
    if (need == 0 || need > vcam::VCamV2MaxFrameSize) return false;
    if (m.frameW != w || m.frameH != h || m.buf.size() < need) {
        try {
            m.buf.resize((size_t)need);
        } catch (...) {
            return false;
        }
        m.frameW = w;
        m.frameH = h;
    }
    return true;
}

bool WriteOne(Machine& m)
{
    if (!m.writerOpen) return false;
    if (m.nativeKnown)
        return m.writer.WriteFrameNative(m.buf.data(), (int)(m.frameW * 4),
                                         m.frameW, m.frameH);
    return m.writer.WriteFrame(m.buf.data(), (int)vcam::VCamStride);
}

// Один кадр из src: натив при известном NativeSize, иначе legacy 720p-путь
// (старт 720p, пока размер неизвестен — video до первого кадра; v2 при этом
// зеркалит 720p). false = кадра сейчас нет (caller ждёт/уходит в fallback).
bool RenderOne(IFrameSource* src, Machine& m, std::wstring& rerr)
{
    uint32_t nw = 0, nh = 0;
    if (src->NativeSize(nw, nh) && nw != 0 && nh != 0 &&
        nw <= vcam::VCamNativeCapW && nh <= vcam::VCamNativeCapH &&
        EnsureFrameBuf(m, nw, nh)) {
        if (!src->Render(m.buf.data(), (int)(nw * 4), nw, nh, rerr)) return false;
        m.nativeKnown = true;
        return true;
    }
    if (!EnsureFrameBuf(m, vcam::VCamWidth, vcam::VCamHeight)) {
        rerr = L"frame buffer alloc failed";
        return false;
    }
    if (!src->Render(m.buf.data(), (int)vcam::VCamStride, rerr)) return false;
    m.nativeKnown = false;
    return true;
}

void EnterFallback(Machine& m, const std::wstring& reason)
{
    CloseSource(m);
    m.phase = Phase::Fallback;
    m.nextAttempt = GetTickCount64() + kFallbackRetryMs;
    Log(L"[cli] no signal: %s (frame is not written - camera fallback)", reason.c_str());
}

void BeginSwitch(Machine& m, const SourceConfig& want, const std::wstring& quality)
{
    const std::wstring wantLabel = TargetLabel(want);
    const std::wstring wasLabel = m.hasTarget ? TargetLabel(m.target) : L"-";
    Log(L"[cli] switch: type=%s path=%s quality=%s (was type=%s path=%s quality=%s)",
        want.type.c_str(), wantLabel.c_str(), quality.c_str(),
        m.hasTarget ? m.target.type.c_str() : L"-", wasLabel.c_str(),
        m.hasTarget ? m.quality.c_str() : L"-");
    CloseSource(m);
    m.target = want;
    // Нормализация как в FrameWriter::SetQuality/Settings::ParseQuality:
    // только fixed720p проходит, остальное -> source.
    m.quality = (quality == L"fixed720p") ? L"fixed720p" : L"source";
    m.writer.SetQuality(m.quality);
    m.nativeKnown = false; // размер нового источника неизвестен -> старт 720p
    m.hasTarget = true;
    m.phase = Phase::Switch;
    m.switchStart = GetTickCount64();
    m.nextAttempt = m.switchStart;
}

// Шаг state machine (ядро то же, что у VCamVideoStreamProducer). Возвращает, сколько
// мс можно ждать до следующего шага.
DWORD Step(Machine& m)
{
    ULONGLONG now = GetTickCount64();
    switch (m.phase) {
    case Phase::Switch: {
        if (!m.src) {
            if (now < m.nextAttempt) {
                FlushOrSleep(m.writer);
                return 0;
            }
            auto cand = CreateSource(m.target.type);
            if (!cand) {
                EnterFallback(m, L"unknown source type: " + m.target.type);
                return 0;
            }
            std::wstring err;
            if (cand->Open(m.target, err)) {
                m.src = std::move(cand);
                Log(L"[cli] source opened: type=%s path=%s",
                    m.target.type.c_str(), TargetLabel(m.target).c_str());
            } else {
                m.nextAttempt = now + kOpenRetryMs;
                Log(L"[cli] open failed: %s", err.c_str());
                if (now - m.switchStart >= kSwitchWindowMs) {
                    EnterFallback(m, L"open failed: " + err);
                    return 0;
                }
                FlushOrSleep(m.writer);
                return 0;
            }
        }

        std::wstring rerr;
        if (RenderOne(m.src.get(), m, rerr)) {
            bool written = WriteOne(m);
            m.phase = Phase::Active;
            Log(L"[cli] active: type=%s path=%s%s [%s %ux%u quality=%s]",
                m.target.type.c_str(), TargetLabel(m.target).c_str(),
                written ? L"" : L" (write failed)",
                m.nativeKnown ? L"native" : L"720p",
                m.frameW, m.frameH, m.quality.c_str());
            return 0;
        }
        if (now - m.switchStart >= kSwitchWindowMs) {
            EnterFallback(m, L"source produces no frames: " + (rerr.empty() ? L"?" : rerr));
            return 0;
        }
        FlushOrSleep(m.writer);
        return 0;
    }

    case Phase::Active: {
        std::wstring rerr;
        if (m.src && RenderOne(m.src.get(), m, rerr)) {
            if (WriteOne(m))
                return 0;
            Sleep(kFrameMs);
            return 0;
        }
        EnterFallback(m, rerr.empty() ? L"source produces no frames" : rerr);
        return 0;
    }

    case Phase::Fallback: {
        if (now >= m.nextAttempt) {
            m.nextAttempt = now + kFallbackRetryMs;
            auto cand = CreateSource(m.target.type);
            if (cand) {
                std::wstring err;
                if (cand->Open(m.target, err)) {
                    std::wstring rerr;
                    if (RenderOne(cand.get(), m, rerr)) {
                        m.src = std::move(cand);
                        WriteOne(m);
                        m.phase = Phase::Active;
                        Log(L"[cli] signal restored: type=%s path=%s [%s %ux%u quality=%s]",
                            m.target.type.c_str(), TargetLabel(m.target).c_str(),
                            m.nativeKnown ? L"native" : L"720p",
                            m.frameW, m.frameH, m.quality.c_str());
                        return 0;
                    }
                } else {
                    Log(L"[cli] retry open failed: %s", err.c_str());
                }
                cand->Close();
            }
        }
        DWORD wait = 0;
        if (m.nextAttempt > now) wait = (DWORD)(m.nextAttempt - now);
        return wait > 500 ? 500 : wait;
    }
    }
    return 0;
}

SourceConfig ResolveTarget(const Settings& s)
{
    SourceConfig want = ToSourceConfig(s, g_fixType.empty() ? s.sourceType : g_fixType);
    if (!g_fixPath.empty()) want.path = g_fixPath;
    // --device: только id, имя из settings не подмешиваем — иначе несовпадающий id
    // мог бы матчиться по имени чужой/устаревшей камеры и открыть не то устройство.
    if (g_fixDevice) want.camName.clear();
    return want;
}

DWORD WINAPI WorkerProc(LPVOID)
{
    Machine m;
    m.buf.resize(vcam::VCamFrameSize);

    std::wstring err;
    m.writerOpen = m.writer.Open(err);
    if (m.writerOpen) {
        Log(L"[cli] shared memory writer ready (%s)", m.writer.SectionOpenedAs().c_str());
        if (m.writer.IsV2Open())
            Log(L"[cli] v2 writer ready (%s)", m.writer.V2SectionOpenedAs().c_str());
        else
            Log(L"[cli] v2 writer unavailable (v1-only mode)");
    } else {
        m.writerErr = err.empty() ? L"writer open failed" : err;
        Log(L"[cli] writer open failed: %s", err.c_str());
    }

    HANDLE waits[2] = { static_cast<HANDLE>(g_stop), static_cast<HANDLE>(g_dirty) };
    bool first = true;
    bool settingsMissingLogged = false;
    DWORD timeout = 0;
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (r == WAIT_OBJECT_0) break;
        if (first || r == WAIT_OBJECT_0 + 1) {
            first = false;
            Settings s;
            if (!g_watcher.Current(s)) {
                s = Settings();
                if (!settingsMissingLogged) {
                    settingsMissingLogged = true;
                    Log(L"[cli] settings not loaded - using defaults");
                }
            }
            SourceConfig want = ResolveTarget(s);
            if (!m.hasTarget || want != m.target || s.quality != m.quality)
                BeginSwitch(m, want, s.quality);
        }
        timeout = Step(m);
    }

    CloseSource(m);
    m.writer.Close();
    Log(L"[cli] writer closed, worker stopped");
    return 0;
}

void OnSettingsChanged(const Settings&) { SetEvent(static_cast<HANDLE>(g_dirty)); }

BOOL WINAPI OnConsoleCtrl(DWORD type)
{
    Log(L"[cli] stop requested (console ctrl type=%lu)", (unsigned long)type);
    if (static_cast<HANDLE>(g_stop) != nullptr) SetEvent(static_cast<HANDLE>(g_stop));
    return TRUE; // не умирать мгновенно: даём основному потоку закрыть writer
}

void PrintHelp()
{
    Log(L"VCamProducerCli - console host for the VCam frame producer (debug/e2e).");
    Log(L"");
    Log(L"Usage:");
    Log(L"  VCamProducerCli.exe run [--type static|video|camera] [--path <file>]");
    Log(L"                           [--device <id>] [--settings <path>]");
    Log(L"  VCamProducerCli.exe list-devices");
    Log(L"  VCamProducerCli.exe list-controls [--device <id>] [--settings <path>]");
    Log(L"  VCamProducerCli.exe get-control --domain <procamp|camera> --id <name|num>");
    Log(L"                           [--device <id>] [--settings <path>]");
    Log(L"  VCamProducerCli.exe set-control --domain <procamp|camera> --id <name|num>");
    Log(L"                           --value <n> [--flags <auto|manual|num>]");
    Log(L"                           [--device <id>] [--settings <path>]");
    Log(L"  VCamProducerCli.exe status [--settings <path>]");
    Log(L"  VCamProducerCli.exe            (no arguments - this help)");
    Log(L"");
    Log(L"run     Console host without the tray. Same core as VCamVideoStreamProducer:");
    Log(L"        settings.json is watched (500 ms + debounce) and source.type changes");
    Log(L"        hot-switch without restart (last good frame is flushed during the");
    Log(L"        switch); on source errors nothing is written -> camera fallback frame.");
    Log(L"        Frames are rendered at the source native size (WriteFrameNative;");
    Log(L"        video starts at 720p until its first frame reports NativeSize).");
    Log(L"        settings \"quality\" (source|fixed720p) hot-switches via reopen.");
    Log(L"        camera.capture (max|720p|1080p) hot-switches via reopen (physical capture height).");
    Log(L"        Logs go to stdout at 30 FPS pacing. Stop with Ctrl+C, Ctrl+Break or Esc.");
    Log(L"  --type <static|video|camera>  Fixes the source type for this run: source.type");
    Log(L"                         changes in settings.json are ignored while running.");
    Log(L"  --path <file>          Fixes the source path for this run: path changes in");
    Log(L"                         settings.json are ignored while running.");
    Log(L"  --device <id>          Camera device for --type camera (required together");
    Log(L"                         with --type camera): fixes SourceConfig.path = id and");
    Log(L"                         clears camName, so only the exact device id matches");
    Log(L"                         (the camera section of settings.json is ignored).");
    Log(L"  Omitted --type/--path/--device keep following settings.json live");
    Log(L"  (hot-switch works for them). All other parameters (scaleMode/crop*) always");
    Log(L"  come from settings.json, even for a fixed type. --type camera without");
    Log(L"  --device uses the settings camera section (empty section -> NO SIGNAL).");
    Log(L"  --settings <path>      Alternate settings file");
    Log(L"                         (default %%APPDATA%%\\VCam\\settings.json).");
    Log(L"");
    Log(L"list-devices  Enumerate camera devices. stdout: one device per line as");
    Log(L"        \"<id>\\t<name>\" (UTF-8 - machine-readable, parse stdout only); the");
    Log(L"        human-readable header/notes go to stderr. No devices -> no stdout");
    Log(L"        rows, message on stderr, exit 0.");
    Log(L"");
    Log(L"list-controls  Introspect physical camera controls (IAMVideoProcAmp +");
    Log(L"        IAMCameraControl) WITHOUT starting the writer/run. Device: --device");
    Log(L"        <id> or the camera section of settings.json. stdout: one control per");
    Log(L"        line as \"<domain>\\t<name>\\t<id>\\t<min>\\t<max>\\t<step>\\t<def>");
    Log(L"        \\t<caps>\\t<cur>\\t<flags>\\t<supported>\" (UTF-8, parse stdout only;");
    Log(L"        header goes to stderr). No device/IAM -> no stdout rows, exit 0.");
    Log(L"        Domain: procamp (brightness..gain) or camera (pan..focus); flags:");
    Log(L"        1=auto, 2=manual; supported=1 if the driver gives a range.");
    Log(L"");
    Log(L"get-control  Read one control: --domain + --id (name or numeric id).");
    Log(L"        stdout: \"<cur>\\t<flags>\" (UTF-8). Unsupported -> stderr, exit 1.");
    Log(L"");
    Log(L"set-control  Apply one control: --domain + --id + --value <n>, optional");
    Log(L"        --flags <auto|manual|num> (default manual; a manual set over auto");
    Log(L"        mode resets the mode to manual first, or the driver ignores it).");
    Log(L"        stdout: \"<applied>\\t<flags>\" re-read from the device (UTF-8).");
    Log(L"");
    Log(L"status  Print settings (path, schema, source.type, quality, camera.capture,");
    Log(L"        static/video/camera sections, autostart), host state (VCamVideoStreamProducer.Instance");
    Log(L"        mutex) and the shared memory writer sections (v1 + v2: version,");
    Log(L"        size, seq growth).");
    Log(L"");
    Log(L"If VCamVideoStreamProducer.exe (tray host) is already running, `run` warns on");
    Log(L"stderr and continues - two producers writing the same shared memory is");
    Log(L"allowed for debugging, but frames will interleave.");
}

struct Options {
    bool run = false;
    bool status = false;
    bool listDevices = false;
    bool listControls = false;
    bool getControl = false;
    bool setControl = false;
    bool help = false;
    std::wstring type;
    std::wstring path;
    std::wstring device;
    std::wstring settings;
    std::wstring domain;
    std::wstring ctlId;
    std::wstring value;
    std::wstring flags;
};

bool ParseArgs(int argc, wchar_t* argv[], Options& o, std::wstring& err)
{
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"run") o.run = true;
        else if (a == L"status") o.status = true;
        else if (a == L"list-devices") o.listDevices = true;
        else if (a == L"list-controls") o.listControls = true;
        else if (a == L"get-control") o.getControl = true;
        else if (a == L"set-control") o.setControl = true;
        else if (a == L"--help" || a == L"-h" || a == L"/?" || a == L"-?") o.help = true;
        else if (a == L"--type" || a == L"--path" || a == L"--device" ||
                 a == L"--settings" || a == L"--domain" || a == L"--id" ||
                 a == L"--value" || a == L"--flags") {
            if (i + 1 >= argc) { err = a + L" requires a value"; return false; }
            std::wstring v = argv[++i];
            if (a == L"--type") {
                if (v != L"static" && v != L"video" && v != L"camera") {
                    err = L"--type must be 'static', 'video' or 'camera', got: " + v;
                    return false;
                }
                o.type = v;
            } else if (a == L"--path") {
                o.path = v;
            } else if (a == L"--device") {
                if (v.empty()) { err = L"--device requires a non-empty value"; return false; }
                o.device = v;
            } else if (a == L"--domain") {
                o.domain = v;
            } else if (a == L"--id") {
                o.ctlId = v;
            } else if (a == L"--value") {
                o.value = v;
            } else if (a == L"--flags") {
                o.flags = v;
            } else {
                o.settings = v;
            }
        } else {
            err = L"unknown argument: " + a;
            return false;
        }
    }
    return true;
}

int CmdStatus(const std::wstring& settingsPath)
{
    Log(L"settings: %s", settingsPath.c_str());
    std::string raw;
    bool fileOk = ReadSmallFile(settingsPath, raw);
    Settings s;
    bool loaded = fileOk && s.Load(settingsPath);
    if (!loaded) {
        Log(L"settings state: missing/unreadable (values below are defaults)");
    } else {
        Log(L"settings state: loaded (%s)",
            LooksNewSchema(raw) ? L"new schema"
                                : L"legacy schema (migrated on read, file untouched)");
    }
    Log(L"source.type: %s", s.sourceType.c_str());
    Log(L"quality: %s", s.quality.c_str());
    Log(L"static.path: %s", s.st.path.empty() ? L"(empty)" : s.st.path.c_str());
    Log(L"static.scaleMode: %s", s.st.scaleMode.c_str());
    Log(L"static.crop: X=%d Y=%d W=%d H=%d keepAspect=%s", s.st.cropX, s.st.cropY,
        s.st.cropW, s.st.cropH, s.st.cropKeepAspect ? L"true" : L"false");
    Log(L"video.path: %s", s.video.path.empty() ? L"(empty)" : s.video.path.c_str());
    Log(L"camera.id: %s", s.cam.id.empty() ? L"(empty)" : s.cam.id.c_str());
    Log(L"camera.name: %s", s.cam.name.empty() ? L"(empty)" : s.cam.name.c_str());
    Log(L"camera.capture: %s", s.cam.capture.c_str());
    if (s.sourceType == L"camera") {
        // Человекочитаемая метка активного источника: имя, иначе id (см. TargetLabel).
        Log(L"camera.source: %s",
            !s.cam.name.empty() ? s.cam.name.c_str()
                                 : (s.cam.id.empty() ? L"(empty)" : s.cam.id.c_str()));
    }
    Log(L"autostart: %s", s.autostart ? L"true" : L"false");
    Log(L"host VCamVideoStreamProducer: %s",
        HostRunning() ? L"running" : L"not running");
    PrintSectionState();
    PrintV2SectionState();
    return 0;
}

// list-devices: stdout = строки "<id>\t<name>" (UTF-8, парсит C#/UI), шапка и
// сообщения — в stderr, чтобы не ломать парсинг stdout. Нет устройств -> пустой
// stdout + сообщение в stderr, exit 0.
int CmdListDevices()
{
    std::vector<CameraDeviceInfo> devs = EnumerateCameraDevices();
    if (devs.empty()) {
        LogErr(L"[cli] no camera devices found (0)");
        return 0;
    }
    LogErr(L"[cli] %d camera device(s) - stdout rows: <id>\\t<name>",
           (int)devs.size());
    for (const CameraDeviceInfo& d : devs) {
        std::wstring id = d.id;
        std::wstring name = d.name;
        // Таб/перевод строки в имени ломал бы построчный парсинг stdout.
        for (wchar_t& c : id)
            if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        for (wchar_t& c : name)
            if (c == L'\t' || c == L'\n' || c == L'\r') c = L' ';
        Log(L"%s\t%s", id.c_str(), name.c_str());
    }
    return 0;
}

// Устройство для control-команд: --device <id> или секция camera из settings.
// Пусто (оба пустые) -> ошибка выбора, НЕ первое устройство.
bool ResolveControlDevice(const std::wstring& settingsPath,
                          const std::wstring& deviceOverride, std::wstring& id,
                          std::wstring& name)
{
    if (!deviceOverride.empty()) {
        id = deviceOverride;
        name.clear();
        return true;
    }
    Settings s;
    if (s.Load(settingsPath)) {
        id = s.cam.id;
        name = s.cam.name;
    }
    return !id.empty() || !name.empty();
}

const wchar_t* ControlDomainName(CameraControlDomain d)
{
    return d == CameraControlDomain::ProcAmp ? L"procamp" : L"camera";
}

int CmdListControls(const std::wstring& settingsPath,
                    const std::wstring& deviceOverride)
{
    std::wstring id, name;
    if (!ResolveControlDevice(settingsPath, deviceOverride, id, name)) {
        LogErr(L"[cli] list-controls: camera not selected (--device or the "
               L"camera section of settings.json)");
        return 1;
    }
    CameraControls ctl;
    std::wstring err;
    if (!ctl.Open(id, name, err)) {
        LogErr(L"[cli] list-controls: %s", err.c_str());
        return 1;
    }
    std::vector<CameraControlDesc> v = ctl.List();
    ctl.Close();
    if (v.empty()) {
        LogErr(L"[cli] list-controls: no IAMVideoProcAmp/IAMCameraControl on this "
               L"device (empty, not an error)");
        return 0;
    }
    LogErr(L"[cli] %d control(s) - stdout rows: "
           L"<domain>\\t<name>\\t<id>\\t<min>\\t<max>\\t<step>\\t<def>\\t<caps>"
           L"\\t<cur>\\t<flags>\\t<supported>",
            (int)v.size());
    for (const CameraControlDesc& d : v) {
        Log(L"%s\t%s\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%ld\t%d",
            ControlDomainName(d.domain), d.name.c_str(), d.id, d.minValue,
            d.maxValue, d.step, d.defaultValue, d.capsFlags, d.curValue,
            d.curFlags, d.supported ? 1 : 0);
    }
    return 0;
}

bool ParseLongArg(const std::wstring& s, long& out)
{
    if (s.empty()) return false;
    wchar_t* end = nullptr;
    out = wcstol(s.c_str(), &end, 10);
    return end != nullptr && *end == L'\0';
}

int CmdGetControl(const std::wstring& settingsPath,
                  const std::wstring& deviceOverride, const std::wstring& domain,
                  const std::wstring& ctlId)
{
    CameraControlDomain dom;
    if (!ParseCameraControlDomain(domain, dom)) {
        LogErr(L"error: --domain must be 'procamp' or 'camera', got: %s",
               domain.c_str());
        return 2;
    }
    long propId = 0;
    std::wstring propName;
    if (!ParseCameraControlId(dom, ctlId, propId, propName)) {
        LogErr(L"error: unknown --id for domain %s: %s", domain.c_str(),
               ctlId.c_str());
        return 2;
    }
    std::wstring id, name;
    if (!ResolveControlDevice(settingsPath, deviceOverride, id, name)) {
        LogErr(L"[cli] get-control: camera not selected (--device or the camera "
               L"section of settings.json)");
        return 1;
    }
    CameraControls ctl;
    std::wstring err;
    if (!ctl.Open(id, name, err)) {
        LogErr(L"[cli] get-control: %s", err.c_str());
        return 1;
    }
    CameraControlDesc d;
    bool ok = ctl.Get(dom, propId, d);
    ctl.Close();
    if (!ok || !d.supported) {
        LogErr(L"[cli] get-control: %s/%s is not supported by this device",
               domain.c_str(), propName.c_str());
        return 1;
    }
    LogErr(L"[cli] %s/%s range [%ld..%ld] step %ld def %ld caps %ld",
           domain.c_str(), propName.c_str(), d.minValue, d.maxValue, d.step,
           d.defaultValue, d.capsFlags);
    Log(L"%ld\t%ld", d.curValue, d.curFlags);
    return 0;
}

int CmdSetControl(const std::wstring& settingsPath,
                  const std::wstring& deviceOverride, const std::wstring& domain,
                  const std::wstring& ctlId, const std::wstring& value,
                  const std::wstring& flags)
{
    CameraControlDomain dom;
    if (!ParseCameraControlDomain(domain, dom)) {
        LogErr(L"error: --domain must be 'procamp' or 'camera', got: %s",
               domain.c_str());
        return 2;
    }
    long propId = 0;
    std::wstring propName;
    if (!ParseCameraControlId(dom, ctlId, propId, propName)) {
        LogErr(L"error: unknown --id for domain %s: %s", domain.c_str(),
               ctlId.c_str());
        return 2;
    }
    long val = 0;
    if (!ParseLongArg(value, val)) {
        LogErr(L"error: --value must be an integer, got: %s", value.c_str());
        return 2;
    }
    long fl = 0x2; // manual по умолчанию (см. CameraControls::Set)
    if (!flags.empty() && !ParseCameraControlFlags(dom, flags, fl)) {
        LogErr(L"error: --flags must be 'auto', 'manual' or a number, got: %s",
               flags.c_str());
        return 2;
    }
    std::wstring id, name;
    if (!ResolveControlDevice(settingsPath, deviceOverride, id, name)) {
        LogErr(L"[cli] set-control: camera not selected (--device or the camera "
               L"section of settings.json)");
        return 1;
    }
    CameraControls ctl;
    std::wstring err;
    if (!ctl.Open(id, name, err)) {
        LogErr(L"[cli] set-control: %s", err.c_str());
        return 1;
    }
    long applied = 0, appliedFlags = 0;
    HRESULT hr = ctl.Set(dom, propId, val, fl, applied, appliedFlags);
    ctl.Close();
    if (FAILED(hr)) {
        LogErr(L"[cli] set-control: %s/%s failed: 0x%08X", domain.c_str(),
               propName.c_str(), (unsigned)hr);
        return 1;
    }
    LogErr(L"[cli] %s/%s set %ld (flags %ld) -> applied %ld (flags %ld)",
           domain.c_str(), propName.c_str(), val, fl, applied, appliedFlags);
    Log(L"%ld\t%ld", applied, appliedFlags);
    return 0;
}

int CmdRun(const std::wstring& settingsPath)
{
    if (HostRunning()) {
        LogErr(L"[cli] WARNING: VCamVideoStreamProducer host is running - two "
               L"producers will write the same shared memory (debug mode, continuing)");
    }

    g_stop.Attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    g_dirty.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (static_cast<HANDLE>(g_stop) == nullptr ||
        static_cast<HANDLE>(g_dirty) == nullptr) {
        LogErr(L"[cli] CreateEvent failed: %lu", GetLastError());
        g_dirty.Close();
        g_stop.Close();
        return 1;
    }
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    Log(L"[cli] VCamProducerCli starting");
    Log(L"[cli] settings: %s", settingsPath.c_str());
    if (!g_fixType.empty() || !g_fixPath.empty()) {
        Log(L"[cli] overrides fixed for this run: type=%s path=%s",
            g_fixType.empty() ? L"(from settings)" : g_fixType.c_str(),
            g_fixPath.empty() ? L"(from settings)" : g_fixPath.c_str());
    }
    if (!g_watcher.Start(settingsPath, OnSettingsChanged)) {
        Log(L"[cli] settings watcher not started (settings path is empty?)");
    } else {
        Log(L"[cli] watching: %s", settingsPath.c_str());
    }

    g_worker.Attach(CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr));
    if (static_cast<HANDLE>(g_worker) == nullptr) {
        LogErr(L"[cli] CreateThread(worker) failed: %lu", GetLastError());
        g_watcher.Stop();
        g_dirty.Close();
        g_stop.Close();
        return 1;
    }

    Log(L"[cli] running @ 30 FPS - stop: Ctrl+C / Ctrl+Break / Esc");

    for (;;) {
        if (WaitForSingleObject(static_cast<HANDLE>(g_stop), 50) == WAIT_OBJECT_0) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Log(L"[cli] stop requested (Esc)");
            SetEvent(g_stop);
            break;
        }
    }

    Log(L"[cli] shutting down");
    g_watcher.Stop();
    // g_dirty/g_stop are shared with the worker: join it before close.
    if (WaitForSingleObject(static_cast<HANDLE>(g_worker), 8000) != WAIT_OBJECT_0) {
        LogErr(L"[cli] worker did not stop in 8000 ms; waiting indefinitely");
        WaitForSingleObject(static_cast<HANDLE>(g_worker), INFINITE);
    }
    g_worker.Close();
    g_dirty.Close();
    g_stop.Close();
    Log(L"[cli] exit");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    Options o;
    std::wstring err;
    if (!ParseArgs(argc, argv, o, err)) {
        LogErr(L"error: %s", err.c_str());
        PrintHelp();
        return 2;
    }
    if (o.help) {
        PrintHelp();
        return 0;
    }
    int cmdCount = (o.run ? 1 : 0) + (o.status ? 1 : 0) + (o.listDevices ? 1 : 0) +
                   (o.listControls ? 1 : 0) + (o.getControl ? 1 : 0) +
                   (o.setControl ? 1 : 0);
    if (cmdCount == 0) {
        if (argc <= 1) {
            PrintHelp();
            return 0;
        }
        LogErr(L"error: expected command 'run', 'status', 'list-devices', "
               L"'list-controls', 'get-control' or 'set-control'");
        PrintHelp();
        return 2;
    }
    if (cmdCount > 1) {
        LogErr(L"error: commands 'run', 'status', 'list-devices', 'list-controls', "
               L"'get-control' and 'set-control' are mutually exclusive");
        return 2;
    }
    const bool isControlCmd = o.listControls || o.getControl || o.setControl;
    if (!o.run && !isControlCmd &&
        (!o.type.empty() || !o.path.empty() || !o.device.empty())) {
        LogErr(L"error: --type/--path/--device are only valid with 'run'");
        return 2;
    }
    if (isControlCmd && (!o.type.empty() || !o.path.empty())) {
        LogErr(L"error: --type/--path are only valid with 'run'");
        return 2;
    }
    if ((o.getControl || o.setControl) && o.domain.empty()) {
        LogErr(L"error: get-control/set-control require --domain <procamp|camera>");
        return 2;
    }
    if ((o.getControl || o.setControl) && o.ctlId.empty()) {
        LogErr(L"error: get-control/set-control require --id <name|num>");
        return 2;
    }
    if (o.setControl && o.value.empty()) {
        LogErr(L"error: set-control requires --value <n>");
        return 2;
    }
    if (!o.domain.empty() || !o.ctlId.empty() || !o.value.empty() ||
        !o.flags.empty()) {
        if (!isControlCmd) {
            LogErr(L"error: --domain/--id/--value/--flags are only valid with "
                   L"get-control/set-control");
            return 2;
        }
    }
    if (!o.flags.empty() && !o.setControl) {
        LogErr(L"error: --flags is only valid with 'set-control'");
        return 2;
    }
    if (!o.path.empty() && !o.device.empty()) {
        LogErr(L"error: --path and --device are mutually exclusive");
        return 2;
    }
    if (!o.device.empty() && o.type != L"camera" && !isControlCmd) {
        LogErr(L"error: --device requires --type camera (or a control command)");
        return 2;
    }

    if (o.listDevices) return CmdListDevices();

    std::wstring settingsPath =
        o.settings.empty() ? DefaultSettingsPath() : o.settings;
    if (settingsPath.empty()) {
        LogErr(L"error: cannot resolve settings path (APPDATA is empty?)");
        return 1;
    }
    if (o.listControls) return CmdListControls(settingsPath, o.device);
    if (o.getControl)
        return CmdGetControl(settingsPath, o.device, o.domain, o.ctlId);
    if (o.setControl)
        return CmdSetControl(settingsPath, o.device, o.domain, o.ctlId, o.value,
                             o.flags);
    g_fixType = o.type;
    if (!o.device.empty()) {
        g_fixPath = o.device;
        g_fixDevice = true;
    } else {
        g_fixPath = o.path;
    }

    if (o.status) return CmdStatus(settingsPath);
    return CmdRun(settingsPath);
}
