#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "CameraDevices.h"
#include "FrameWriter.h"
#include "ProducerApi.h"
#include "Settings.h"
#include "SettingsWatcher.h"
#include "SharedMemoryContract.h"

namespace {

constexpr wchar_t kHostMutexName[] = L"VCamVideoStreamProducer.Instance";

constexpr DWORD kFrameMs = 33;
constexpr DWORD kSwitchWindowMs = 5000; // окно hot-switch: FlushLast, пока новый источник не готов
constexpr DWORD kOpenRetryMs = 250;
constexpr DWORD kFallbackRetryMs = 1000;

HANDLE g_stop = nullptr;
HANDLE g_dirty = nullptr;
HANDLE g_worker = nullptr;
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
    HANDLE m = OpenMutexW(SYNCHRONIZE, FALSE, kHostMutexName);
    if (!m) return 0;
    DWORD r = WaitForSingleObject(m, 0);
    int running = (r == WAIT_TIMEOUT) ? 1 : 0;
    if (r == WAIT_OBJECT_0 || r == WAIT_ABANDONED) ReleaseMutex(m);
    CloseHandle(m);
    return running;
}

bool ReadSmallFile(const std::wstring& path, std::string& out)
{
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1000000) {
        CloseHandle(h);
        return false;
    }
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    CloseHandle(h);
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
    HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, vcam::VCamSectionName);
    if (!h) {
        Log(L"writer section %s: not open (no producer holds it)", vcam::VCamSectionName);
        return;
    }
    void* p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(vcam::VCamSectionHeader));
    if (!p) {
        Log(L"writer section %s: open (MapView failed: %lu)", vcam::VCamSectionName,
            GetLastError());
        CloseHandle(h);
        return;
    }
    LONGLONG seq1 = reinterpret_cast<vcam::VCamSectionHeader*>(p)->seq;
    Sleep(300);
    LONGLONG seq2 = reinterpret_cast<vcam::VCamSectionHeader*>(p)->seq;
    if (seq2 != seq1) {
        Log(L"writer section %s: open, frames are being written (seq %lld -> %lld)",
            vcam::VCamSectionName, seq1, seq2);
    } else {
        Log(L"writer section %s: open, no new frames in the last 300 ms (seq %lld)",
            vcam::VCamSectionName, seq1);
    }
    UnmapViewOfFile(p);
    CloseHandle(h);
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
    Phase phase = Phase::Switch;
    ULONGLONG switchStart = 0;
    ULONGLONG nextAttempt = 0;
    std::vector<uint8_t> buf;
};

void CloseSource(Machine& m)
{
    if (m.src) {
        m.src->Close();
        m.src.reset();
    }
}

void EnterFallback(Machine& m, const std::wstring& reason)
{
    CloseSource(m);
    m.phase = Phase::Fallback;
    m.nextAttempt = GetTickCount64() + kFallbackRetryMs;
    Log(L"[cli] no signal: %s (frame is not written - camera fallback)", reason.c_str());
}

void BeginSwitch(Machine& m, const SourceConfig& want)
{
    const std::wstring wantLabel = TargetLabel(want);
    const std::wstring wasLabel = m.hasTarget ? TargetLabel(m.target) : L"-";
    Log(L"[cli] switch: type=%s path=%s (was type=%s path=%s)",
        want.type.c_str(), wantLabel.c_str(),
        m.hasTarget ? m.target.type.c_str() : L"-", wasLabel.c_str());
    CloseSource(m);
    m.target = want;
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
        if (m.src->Render(m.buf.data(), (int)vcam::VCamStride, rerr)) {
            bool written = m.writerOpen &&
                           m.writer.WriteFrame(m.buf.data(), (int)vcam::VCamStride);
            m.phase = Phase::Active;
            Log(L"[cli] active: type=%s path=%s%s",
                m.target.type.c_str(), TargetLabel(m.target).c_str(),
                written ? L"" : L" (write failed)");
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
        if (m.src && m.src->Render(m.buf.data(), (int)vcam::VCamStride, rerr)) {
            if (m.writerOpen && m.writer.WriteFrame(m.buf.data(), (int)vcam::VCamStride))
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
                    if (cand->Render(m.buf.data(), (int)vcam::VCamStride, rerr)) {
                        m.src = std::move(cand);
                        if (m.writerOpen)
                            m.writer.WriteFrame(m.buf.data(), (int)vcam::VCamStride);
                        m.phase = Phase::Active;
                        Log(L"[cli] signal restored: type=%s path=%s",
                            m.target.type.c_str(), TargetLabel(m.target).c_str());
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
        Log(L"[cli] shared memory writer ready (Global\\VCam.FrameBuffer.v1)");
    } else {
        m.writerErr = err.empty() ? L"writer open failed" : err;
        Log(L"[cli] writer open failed: %s", err.c_str());
    }

    HANDLE waits[2] = { g_stop, g_dirty };
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
            if (!m.hasTarget || want != m.target) BeginSwitch(m, want);
        }
        timeout = Step(m);
    }

    CloseSource(m);
    m.writer.Close();
    Log(L"[cli] writer closed, worker stopped");
    return 0;
}

void OnSettingsChanged(const Settings&) { SetEvent(g_dirty); }

BOOL WINAPI OnConsoleCtrl(DWORD type)
{
    Log(L"[cli] stop requested (console ctrl type=%lu)", (unsigned long)type);
    if (g_stop) SetEvent(g_stop);
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
    Log(L"  VCamProducerCli.exe status [--settings <path>]");
    Log(L"  VCamProducerCli.exe            (no arguments - this help)");
    Log(L"");
    Log(L"run     Console host without the tray. Same core as VCamVideoStreamProducer:");
    Log(L"        settings.json is watched (500 ms + debounce) and source.type changes");
    Log(L"        hot-switch without restart (last good frame is flushed during the");
    Log(L"        switch); on source errors nothing is written -> camera fallback frame.");
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
    Log(L"status  Print settings (path, schema, source.type, static/video/camera");
    Log(L"        sections, autostart), host state (VCamVideoStreamProducer.Instance");
    Log(L"        mutex) and the shared memory writer section state.");
    Log(L"");
    Log(L"If VCamVideoStreamProducer.exe (tray host) is already running, `run` warns on");
    Log(L"stderr and continues - two producers writing the same shared memory is");
    Log(L"allowed for debugging, but frames will interleave.");
}

struct Options {
    bool run = false;
    bool status = false;
    bool listDevices = false;
    bool help = false;
    std::wstring type;
    std::wstring path;
    std::wstring device;
    std::wstring settings;
};

bool ParseArgs(int argc, wchar_t* argv[], Options& o, std::wstring& err)
{
    for (int i = 1; i < argc; i++) {
        std::wstring a = argv[i];
        if (a == L"run") o.run = true;
        else if (a == L"status") o.status = true;
        else if (a == L"list-devices") o.listDevices = true;
        else if (a == L"--help" || a == L"-h" || a == L"/?" || a == L"-?") o.help = true;
        else if (a == L"--type" || a == L"--path" || a == L"--device" ||
                 a == L"--settings") {
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
    Log(L"static.path: %s", s.st.path.empty() ? L"(empty)" : s.st.path.c_str());
    Log(L"static.scaleMode: %s", s.st.scaleMode.c_str());
    Log(L"static.crop: X=%d Y=%d W=%d H=%d keepAspect=%s", s.st.cropX, s.st.cropY,
        s.st.cropW, s.st.cropH, s.st.cropKeepAspect ? L"true" : L"false");
    Log(L"video.path: %s", s.video.path.empty() ? L"(empty)" : s.video.path.c_str());
    Log(L"camera.id: %s", s.cam.id.empty() ? L"(empty)" : s.cam.id.c_str());
    Log(L"camera.name: %s", s.cam.name.empty() ? L"(empty)" : s.cam.name.c_str());
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

int CmdRun(const std::wstring& settingsPath)
{
    if (HostRunning()) {
        LogErr(L"[cli] WARNING: VCamVideoStreamProducer host is running - two "
               L"producers will write the same shared memory (debug mode, continuing)");
    }

    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_dirty = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stop || !g_dirty) {
        LogErr(L"[cli] CreateEvent failed: %lu", GetLastError());
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

    g_worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (!g_worker) {
        LogErr(L"[cli] CreateThread(worker) failed: %lu", GetLastError());
        g_watcher.Stop();
        CloseHandle(g_dirty);
        g_dirty = nullptr;
        CloseHandle(g_stop);
        g_stop = nullptr;
        return 1;
    }

    Log(L"[cli] running @ 30 FPS - stop: Ctrl+C / Ctrl+Break / Esc");

    for (;;) {
        if (WaitForSingleObject(g_stop, 50) == WAIT_OBJECT_0) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            Log(L"[cli] stop requested (Esc)");
            SetEvent(g_stop);
            break;
        }
    }

    Log(L"[cli] shutting down");
    g_watcher.Stop();
    WaitForSingleObject(g_worker, 8000);
    CloseHandle(g_worker);
    g_worker = nullptr;
    CloseHandle(g_dirty);
    g_dirty = nullptr;
    HANDLE stop = g_stop;
    g_stop = nullptr;
    CloseHandle(stop);
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
    int cmdCount = (o.run ? 1 : 0) + (o.status ? 1 : 0) + (o.listDevices ? 1 : 0);
    if (cmdCount == 0) {
        if (argc <= 1) {
            PrintHelp();
            return 0;
        }
        LogErr(L"error: expected command 'run', 'status' or 'list-devices'");
        PrintHelp();
        return 2;
    }
    if (cmdCount > 1) {
        LogErr(L"error: commands 'run', 'status' and 'list-devices' are mutually exclusive");
        return 2;
    }
    if (!o.run && (!o.type.empty() || !o.path.empty() || !o.device.empty())) {
        LogErr(L"error: --type/--path/--device are only valid with 'run'");
        return 2;
    }
    if (!o.path.empty() && !o.device.empty()) {
        LogErr(L"error: --path and --device are mutually exclusive");
        return 2;
    }
    if (!o.device.empty() && o.type != L"camera") {
        LogErr(L"error: --device requires --type camera");
        return 2;
    }

    if (o.listDevices) return CmdListDevices();

    std::wstring settingsPath =
        o.settings.empty() ? DefaultSettingsPath() : o.settings;
    if (settingsPath.empty()) {
        LogErr(L"error: cannot resolve settings path (APPDATA is empty?)");
        return 1;
    }
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
