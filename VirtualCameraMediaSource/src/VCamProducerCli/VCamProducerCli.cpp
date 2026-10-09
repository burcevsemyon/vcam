#include <windows.h>
#include <atlbase.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "CameraDevices.h"
#include "CameraControls.h"
#include "CliPipelineEngine.h"
#include "ControlServer.h"
#include "FrameWriter.h"
#include "LogFormat.h"
#include "FailOpenFile.h"
#include "RuntimeCounters.h"
#include "CrashDump.h"
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
constexpr DWORD kFrameSettleDelayMs = 300; // пауза для проверки роста seq в секции
constexpr DWORD kLivenessPollMs = 100; // живость: при 30 FPS за 100 мс seq вырастет на ~3 кадра
constexpr DWORD kUiPollMs = 50; // опрос клавиатуры в цикле ожидания остановки

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

// Runtime-логи (run-режим, диагностика команд): единый формат строки
// (Common/LogFormat.h), область [cli]. Вывод команд (help/status/строки
// stdout для парсинга) идёт через Log/LogErr БЕЗ префикса — это данные.
void RunLogv(bool err, const wchar_t* fmt, va_list ap)
{
    wchar_t buf[4096];
    vswprintf_s(buf, fmt, ap);
    std::wstring line = vcam::FormatLogLine(
        err ? vcam::LogLevel::Error : vcam::LogLevel::Info, L"cli", buf);
    line += L'\n';
    Emit(err, line);
}

void RunLog(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    RunLogv(false, fmt, ap);
    va_end(ap);
}

void RunLogErr(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    RunLogv(true, fmt, ap);
    va_end(ap);
}

// Debug-строка (P1.1): только при VCAM_DEBUG=1, иначе no-op без форматирования.
void RunLogDebug(const wchar_t* fmt, ...)
{
    if (!vcam::IsDebugEnabled()) return;
    va_list ap;
    va_start(ap, fmt);
    wchar_t buf[4096];
    vswprintf_s(buf, fmt, ap);
    va_end(ap);
    std::wstring line = vcam::FormatLogLine(vcam::LogLevel::Debug, L"cli", buf);
    line += L'\n';
    Emit(false, line);
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

// --- P0.3: REC/borrow state из транзентов хоста для `status` ---
// Минимальный геттер строкового JSON-значения (без unescape; пути ASCII — достаточно).
bool JsonGetRawString(const std::string& json, const char* key, std::string& out)
{
    out.clear();
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return false;
    size_t c = json.find(':', p + k.size());
    if (c == std::string::npos) return false;
    size_t q1 = json.find('"', c);
    if (q1 == std::string::npos) return false;
    size_t q2 = json.find('"', q1 + 1);
    if (q2 == std::string::npos) return false;
    out = json.substr(q1 + 1, q2 - q1 - 1);
    return true;
}

bool JsonGetRawBool(const std::string& json, const char* key, bool& out)
{
    out = false;
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return false;
    size_t c = json.find(':', p + k.size());
    if (c == std::string::npos) return false;
    size_t v = c + 1;
    while (v < json.size() && (json[v] == ' ' || json[v] == '\t')) v++;
    if (v + 4 <= json.size() && json.compare(v, 4, "true") == 0) { out = true; return true; }
    if (v + 5 <= json.size() && json.compare(v, 5, "false") == 0) { out = false; return true; }
    return false;
}

std::wstring Utf8ToWideCli(const std::string& u8)
{
    if (u8.empty()) return std::wstring();
    int m = MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), nullptr, 0);
    if (m <= 0) return std::wstring();
    std::wstring w((size_t)m, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, u8.c_str(), (int)u8.size(), &w[0], m);
    return w;
}

// record_state.json: {"recording":true,"path":"...","started":123} (пишет хост).
// Возвращает true, если хост сейчас пишет эфир.
bool ReadActiveRec(std::wstring& path, long long& started)
{
    path.clear(); started = 0;
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::string json;
    if (!ReadSmallFile(std::wstring(appdata) + L"\\VCam\\record_state.json", json)) return false;
    bool rec = false;
    if (!JsonGetRawBool(json, "recording", rec) || !rec) return false;
    std::string p;
    if (JsonGetRawString(json, "path", p)) path = Utf8ToWideCli(p);
    size_t ks = json.find("\"started\"");
    if (ks != std::string::npos) {
        size_t cc = json.find(':', ks);
        if (cc != std::string::npos) started = _atoi64(json.c_str() + cc + 1);
    }
    return true;
}

// hotkey_state.json: {"borrowed":true,"returnTo":"..."} (пишет хост).
// Возвращает true, если хоткей одолжил video (play-once).
bool ReadActiveBorrow(std::wstring& returnTo)
{
    returnTo.clear();
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return false;
    std::string json;
    if (!ReadSmallFile(std::wstring(appdata) + L"\\VCam\\hotkey_state.json", json)) return false;
    bool b = false;
    if (!JsonGetRawBool(json, "borrowed", b) || !b) return false;
    std::string r;
    if (JsonGetRawString(json, "returnTo", r)) returnTo = Utf8ToWideCli(r);
    return true;
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
    Sleep(kFrameSettleDelayMs);
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
    Sleep(kFrameSettleDelayMs);
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

// --- P1.2: снапшот секции числами для `status --json` (принтеры выше не тронуты) ---
struct SectionNumbers {
    bool open = false;
    std::wstring name;
    LONGLONG seq1 = 0;
    LONGLONG seq2 = 0;
    bool growing() const { return open && seq2 != seq1; }
};

// Открывает секцию (перебор Global/Local), читает seq дважды (пауза 300 мс).
// checkMagic = true для v2 (валидация magic; битый заголовок = not open).
bool SnapshotSectionNumbers(const wchar_t* fullBaseName, bool checkMagic, SectionNumbers& out)
{
    out = SectionNumbers{};
    const wchar_t* pSep = wcschr(fullBaseName, L'\\');
    const wchar_t* baseName = (pSep != nullptr) ? pSep + 1 : fullBaseName;
    const wchar_t* prefixes[2] = { L"Global\\", L"Local\\" };
    wchar_t sectionName[128] = {};
    ATL::CHandle h;
    for (int i = 0; i < 2 && !h; i++) {
        swprintf_s(sectionName, L"%s%s", prefixes[i], baseName);
        h.Attach(OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName));
    }
    if (!h) return false;
    void* raw = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(vcam::VCamSectionHeader));
    vcam::MappedViewOfFilePtr view(raw);
    if (!view) return false;
    auto* hdr = view.GetAs<vcam::VCamSectionHeader>();
    if (checkMagic && hdr->magic != vcam::VCamMagic) return false;
    out.open = true;
    out.name = sectionName;
    out.seq1 = hdr->seq;
    Sleep(300);
    out.seq2 = view.GetAs<vcam::VCamSectionHeader>()->seq;
    return true;
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
    CliPipelineEngine engine;
    engine.Open();

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
                    RunLog(L"settings not loaded - using defaults");
                }
            }
            SourceConfig want = ResolveTarget(s);
            // P1.4: что применено — на старте и каждом перечитывании настроек (Info).
            RunLog(L"config applied: type=%s path=%s quality=%s record=%s",
                want.type.c_str(), TargetLabel(want).c_str(), s.quality.c_str(),
                s.record.path.empty() ? L"(default)" : s.record.path.c_str());
            if (!engine.HasTarget() || want != engine.Target() || s.quality != engine.Quality()) {
                RunLogDebug(L"apply target: type=%s path=%s camName=%s capture=%s scaleMode=%s "
                            L"crop=(%d,%d,%d,%d)%s quality=%s playOnce=%d loop=%d",
                    want.type.c_str(), want.path.c_str(), want.camName.c_str(),
                    want.capture.c_str(), want.scaleMode.c_str(), want.cropX, want.cropY,
                    want.cropW, want.cropH, want.cropKeepAspect ? L" keepAspect" : L"",
                    s.quality.c_str(), want.playOnce ? 1 : 0, want.loop ? 1 : 0);
                engine.SetTarget(want, s.quality);
            }
        }
        timeout = engine.Step();
    }

    engine.Close();
    RunLog(L"writer closed, worker stopped");
    return 0;
}

void OnSettingsChanged(const Settings&) { SetEvent(static_cast<HANDLE>(g_dirty)); }

BOOL WINAPI OnConsoleCtrl(DWORD type)
{
    RunLog(L"stop requested (console ctrl type=%lu)", (unsigned long)type);
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
    Log(L"  VCamProducerCli.exe status [--settings <path>] [--json] [--ready]");
    Log(L"  VCamProducerCli.exe diag [--out <dir>] [--settings <path>]");
    Log(L"  VCamProducerCli.exe            (no arguments - this help)");
    Log(L"");
    Log(L"run     Console host without the tray. Same core as VCamVideoStreamProducer:");
    Log(L"        settings.json is watched (500 ms + debounce) and source.type changes");
    Log(L"        hot-switch without restart (last good frame is flushed during the");
    Log(L"        switch); on source errors nothing is written -> camera fallback frame.");
    Log(L"        Frames are rendered at the source native size (WriteFrameNative;");
    Log(L"        video starts at 720p until its first frame reports NativeSize).");
    Log(L"        settings \"quality\" (source|fixed1080p|fixed720p) hot-switches via reopen.");
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
    Log(L"diag  Collect a diagnostic bundle for support: versions, status +");
    Log(L"        status.json, settings.json + state/counters, log tails (200KB),");
    Log(L"        token state, autostart task, crash-dump list -> directory");
    Log(L"        (default %TEMP%\\VCamDiag_<ts>, or --out <dir>). Read-only,");
    Log(L"        partial bundle still useful. Attach it (or zip) to the issue.");
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
    bool json = false; // --json: только со status, метрики машиночитаемо (P1.2)
    bool ready = false; // --ready: только со status, быстрый readiness-gate (P1.3)
    bool diag = false; // diag: пакет диагностики для поддержки (P2.3)
    std::wstring outDir; // --out: каталог пакета diag (default %TEMP%\VCamDiag_<ts>)
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
        else if (a == L"--json") o.json = true;
        else if (a == L"--ready") o.ready = true;
        else if (a == L"diag") o.diag = true;
        else if (a == L"--type" || a == L"--path" || a == L"--device" ||
                 a == L"--settings" || a == L"--out" || a == L"--domain" || a == L"--id" ||
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
            } else if (a == L"--out") {
                if (v.empty()) { err = L"--out requires a non-empty value"; return false; }
                o.outDir = v;
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

// P1.3: быстрая проверка готовности писателя для readiness-gate.
// true = v1-секция открыта и seq РАСТЁТ за 100 мс (живой писатель пишет кадры).
// Stale-секция (убитый продьюсер, seq заморожен) → false. Без флага — мгновенно,
// с живым писателем ~100 мс (vs 600 мс у полного status).
bool CheckWriterReady(LONGLONG& seqOut, std::wstring& nameOut)
{
    seqOut = 0; nameOut.clear();
    const wchar_t* pSep = wcschr(vcam::VCamSectionName, L'\\');
    const wchar_t* baseName = (pSep != nullptr) ? pSep + 1 : vcam::VCamSectionName;
    const wchar_t* prefixes[2] = { L"Global\\", L"Local\\" };
    wchar_t sectionName[128] = {};
    ATL::CHandle h;
    for (int i = 0; i < 2 && !h; i++) {
        swprintf_s(sectionName, L"%s%s", prefixes[i], baseName);
        h.Attach(OpenFileMappingW(FILE_MAP_READ, FALSE, sectionName));
    }
    if (!h) return false;
    void* raw = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(vcam::VCamSectionHeader));
    vcam::MappedViewOfFilePtr view(raw);
    if (!view) return false;
    auto* hdr = view.GetAs<vcam::VCamSectionHeader>();
    LONGLONG seq1 = hdr->seq;
    if (seq1 <= 0) return false;
    Sleep(kLivenessPollMs);
    LONGLONG seq2 = hdr->seq; // seq volatile — свежее чтение
    if (seq2 <= seq1) return false;
    seqOut = seq2;
    nameOut = sectionName;
    return true;
}

// P1.2: `status --json` — метрики числами для скриптов (e2e/диагностика).
// Одна JSON-строка в stdout (raw Log, без префикса — это данные). Строки только
// из фиксированных ASCII-наборов (type/quality); пути не включаем (экранирование).
int CmdStatusJson(const Settings& s)
{
    int hostRunning = HostRunning() ? 1 : 0;
    SectionNumbers v1, v2;
    SnapshotSectionNumbers(vcam::VCamSectionName, false, v1);
    SnapshotSectionNumbers(vcam::VCamSectionNameV2, true, v2);
    std::wstring recPath; long long recStarted = 0;
    bool recActive = ReadActiveRec(recPath, recStarted);
    long long recDur = 0;
    if (recActive && recStarted > 0) {
        long long now = (long long)time(nullptr);
        if (now > recStarted) recDur = now - recStarted;
    }
    std::wstring borrowTo;
    bool borrowed = ReadActiveBorrow(borrowTo);
    int64_t fo[6] = {};
    bool foOk = vcam::ReadFailOpenCounters(fo);
    vcam::RuntimeCounters rc;
    bool rcOk = vcam::ReadRuntimeCounters(rc);

    wchar_t foPart[256], rcPart[512];
    if (foOk) {
        swprintf_s(foPart, L"{\"settingsJson\":%lld,\"hotkey\":%lld,\"recordStart\":%lld,"
            L"\"sourceOpen\":%lld,\"staleRemoved\":%lld,\"control\":%lld}",
            fo[0], fo[1], fo[2], fo[3], fo[4], fo[5]);
    } else {
        wcscpy_s(foPart, L"null");
    }
    if (rcOk) {
        swprintf_s(rcPart, L"{\"switches\":%lld,\"fallbacks\":%lld,"
            L"\"frameMinUs\":%lld,\"frameMaxUs\":%lld,\"frameAvgUs\":%lld,"
            L"\"frameCount\":%lld,\"recFrames\":%llu,\"recDropped\":%llu}",
            rc.switches, rc.fallbacks, rc.frameMinUs, rc.frameMaxUs,
            rc.frameAvgUs, rc.frameCount, rc.recFrames, rc.recDropped);
    } else {
        wcscpy_s(rcPart, L"null");
    }
    Log(L"{\"hostRunning\":%s,\"sourceType\":\"%s\",\"quality\":\"%s\","
        L"\"v1\":{\"open\":%s,\"seq1\":%lld,\"seq2\":%lld,\"growing\":%s},"
        L"\"v2\":{\"open\":%s,\"seq1\":%lld,\"seq2\":%lld,\"growing\":%s},"
        L"\"rec\":{\"active\":%s,\"durationSec\":%lld},"
        L"\"hotkey\":{\"borrowed\":%s},"
        L"\"failopen\":%s,"
        L"\"runtime\":%s}",
        hostRunning ? L"true" : L"false", s.sourceType.c_str(), s.quality.c_str(),
        v1.open ? L"true" : L"false", v1.seq1, v1.seq2,
        (v1.open && v1.seq2 != v1.seq1) ? L"true" : L"false",
        v2.open ? L"true" : L"false", v2.seq1, v2.seq2,
        (v2.open && v2.seq2 != v2.seq1) ? L"true" : L"false",
        recActive ? L"true" : L"false", recDur,
        borrowed ? L"true" : L"false",
        foPart, rcPart);
    return 0;
}

int CmdStatus(const std::wstring& settingsPath, bool asJson, bool asReady)
{
    // P1.3: readiness-gate — быстрая проверка без чтения настроек и sleep.
    if (asReady) {
        LONGLONG seq = 0; std::wstring name;
        if (CheckWriterReady(seq, name)) {
            Log(L"ready=1 seq=%lld", seq);
            return 0;
        }
        Log(L"ready=0");
        return 1;
    }
    std::string raw;
    bool fileOk = ReadSmallFile(settingsPath, raw);
    Settings s;
    bool loaded = fileOk && s.Load(settingsPath);
    if (asJson) return CmdStatusJson(s);
    Log(L"settings: %s", settingsPath.c_str());
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
    // P0.3: состояние и деградации одним вызовом (не ломает `frames are being written`).
    {
        std::wstring recPath; long long recStarted = 0;
        if (ReadActiveRec(recPath, recStarted)) {
            Log(L"REC: recording (%s)",
                recPath.empty() ? L"(path?)" : recPath.c_str());
        } else {
            Log(L"REC: idle");
        }
    }
    {
        std::wstring borrowTo;
        if (ReadActiveBorrow(borrowTo)) {
            Log(L"hotkey: borrowed (return to %s)",
                borrowTo.empty() ? L"static" : borrowTo.c_str());
        } else {
            Log(L"hotkey: normal");
        }
    }
    {
        int64_t fo[6] = {};
        if (vcam::ReadFailOpenCounters(fo)) {
            Log(L"fail-open: settingsJson=%lld hotkey=%lld recordStart=%lld "
                L"sourceOpen=%lld staleRemoved=%lld control=%lld",
                fo[0], fo[1], fo[2], fo[3], fo[4], fo[5]);
        } else {
            Log(L"fail-open: n/a (host never reported)");
        }
    }
    return 0;
}

// list-devices: stdout = строки "<id>\t<name>" (UTF-8, парсит C#/UI), шапка и
// сообщения — в stderr, чтобы не ломать парсинг stdout. Нет устройств -> пустой
// stdout + сообщение в stderr, exit 0.
int CmdListDevices()
{
    std::vector<CameraDeviceInfo> devs = EnumerateCameraDevices();
    if (devs.empty()) {
        RunLogErr(L"no camera devices found (0)");
        return 0;
    }
    RunLogErr(L"%d camera device(s) - stdout rows: <id>\\t<name>",
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
        RunLogErr(L"list-controls: camera not selected (--device or the "
               L"camera section of settings.json)");
        return 1;
    }
    CameraControls ctl;
    std::wstring err;
    if (!ctl.Open(id, name, err)) {
        RunLogErr(L"list-controls: %s", err.c_str());
        return 1;
    }
    std::vector<CameraControlDesc> v = ctl.List();
    ctl.Close();
    if (v.empty()) {
        RunLogErr(L"list-controls: no IAMVideoProcAmp/IAMCameraControl on this "
               L"device (empty, not an error)");
        return 0;
    }
    RunLogErr(L"%d control(s) - stdout rows: "
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
        RunLogErr(L"get-control: camera not selected (--device or the camera "
               L"section of settings.json)");
        return 1;
    }
    CameraControls ctl;
    std::wstring err;
    if (!ctl.Open(id, name, err)) {
        RunLogErr(L"get-control: %s", err.c_str());
        return 1;
    }
    CameraControlDesc d;
    bool ok = ctl.Get(dom, propId, d);
    ctl.Close();
    if (!ok || !d.supported) {
        RunLogErr(L"get-control: %s/%s is not supported by this device",
               domain.c_str(), propName.c_str());
        return 1;
    }
    RunLogErr(L"%s/%s range [%ld..%ld] step %ld def %ld caps %ld",
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
        RunLogErr(L"set-control: camera not selected (--device or the camera "
               L"section of settings.json)");
        return 1;
    }
    CameraControls ctl;
    std::wstring err;
    if (!ctl.Open(id, name, err)) {
        RunLogErr(L"set-control: %s", err.c_str());
        return 1;
    }
    long applied = 0, appliedFlags = 0;
    HRESULT hr = ctl.Set(dom, propId, val, fl, applied, appliedFlags);
    ctl.Close();
    if (FAILED(hr)) {
        RunLogErr(L"set-control: %s/%s failed: 0x%08X", domain.c_str(),
               propName.c_str(), (unsigned)hr);
        return 1;
    }
    RunLogErr(L"%s/%s set %ld (flags %ld) -> applied %ld (flags %ld)",
           domain.c_str(), propName.c_str(), val, fl, applied, appliedFlags);
    Log(L"%ld\t%ld", applied, appliedFlags);
    return 0;
}

int CmdRun(const std::wstring& settingsPath)
{
    if (HostRunning()) {
        RunLogErr(L"WARNING: VCamVideoStreamProducer host is running - two "
               L"producers will write the same shared memory (debug mode, continuing)");
    }

    g_stop.Attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    g_dirty.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (static_cast<HANDLE>(g_stop) == nullptr ||
        static_cast<HANDLE>(g_dirty) == nullptr) {
        RunLogErr(L"CreateEvent failed: %lu", GetLastError());
        g_dirty.Close();
        g_stop.Close();
        return 1;
    }
    SetConsoleCtrlHandler(OnConsoleCtrl, TRUE);

    RunLog(L"VCamProducerCli starting");
    RunLog(L"settings: %s", settingsPath.c_str());
    if (!g_fixType.empty() || !g_fixPath.empty()) {
        RunLog(L"overrides fixed for this run: type=%s path=%s",
            g_fixType.empty() ? L"(from settings)" : g_fixType.c_str(),
            g_fixPath.empty() ? L"(from settings)" : g_fixPath.c_str());
    }
    g_watcher.SetLogCallback([](const std::wstring& m) { RunLog(L"%s", m.c_str()); });
    if (!g_watcher.Start(settingsPath, OnSettingsChanged)) {
        RunLog(L"settings watcher not started (settings path is empty?)");
    } else {
        RunLog(L"watching: %s", settingsPath.c_str());
    }

    g_worker.Attach(CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr));
    if (static_cast<HANDLE>(g_worker) == nullptr) {
        RunLogErr(L"CreateThread(worker) failed: %lu", GetLastError());
        g_watcher.Stop();
        g_dirty.Close();
        g_stop.Close();
        return 1;
    }

    RunLog(L"running @ 30 FPS - stop: Ctrl+C / Ctrl+Break / Esc");

    for (;;) {
        if (WaitForSingleObject(static_cast<HANDLE>(g_stop), kUiPollMs) == WAIT_OBJECT_0) break;
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            RunLog(L"stop requested (Esc)");
            SetEvent(g_stop);
            break;
        }
    }

    RunLog(L"shutting down");
    g_watcher.Stop();
    // g_dirty/g_stop are shared with the worker: join it before close.
    if (WaitForSingleObject(static_cast<HANDLE>(g_worker), 8000) != WAIT_OBJECT_0) {
        RunLogErr(L"worker did not stop in 8000 ms; waiting indefinitely");
        WaitForSingleObject(static_cast<HANDLE>(g_worker), INFINITE);
    }
    g_worker.Close();
    g_dirty.Close();
    g_stop.Close();
    RunLog(L"exit");
    return 0;
}

#pragma comment(lib, "Version.lib") // GetFileVersionInfo (P2.3, без правок vcxproj)

// --- P2.3: пакет диагностики (`diag`) — хелперы сбора ---
namespace diag {

const uint64_t kTailBytes = 200ull * 1024; // лимит хвоста лога в пакете

bool EnsureDir(const std::wstring& path)
{
    if (path.empty()) return false;
    for (size_t i = 0; i < path.size(); i++) {
        if ((path[i] == L'\\' || path[i] == L'/') && i >= 3) {
            CreateDirectoryW(path.substr(0, i).c_str(), nullptr); // есть — ok
        }
    }
    return CreateDirectoryW(path.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

// UTF-8 без BOM (для manifest/versions/token/crashes).
bool WriteTextFile(const std::wstring& path, const std::wstring& text)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return false;
    std::string utf8((size_t)(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, &utf8[0], n, nullptr, nullptr);
    HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle hOut(out);
    DWORD w = 0;
    return WriteFile(hOut, utf8.data(), (DWORD)utf8.size(), &w, nullptr) && w == utf8.size();
}

bool GetFileVersionString(const std::wstring& path, std::wstring& out)
{
    out.clear();
    DWORD dummy = 0;
    DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (sz == 0) return false;
    std::vector<BYTE> buf(sz);
    if (!GetFileVersionInfoW(path.c_str(), 0, sz, buf.data())) return false;
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(buf.data(), L"\\", (void**)&fi, &len) || !fi) return false;
    wchar_t v[64];
    swprintf_s(v, L"%u.%u.%u.%u",
        (unsigned)HIWORD(fi->dwFileVersionMS), (unsigned)LOWORD(fi->dwFileVersionMS),
        (unsigned)HIWORD(fi->dwFileVersionLS), (unsigned)LOWORD(fi->dwFileVersionLS));
    out = v;
    return true;
}

std::wstring GetOsVersionString()
{
    std::wstring product, display, build;
    wchar_t buf[256]; DWORD cb;
    cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"ProductName", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS)
        product = buf;
    cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"DisplayVersion", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS)
        display = buf;
    cb = sizeof(buf);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion",
                     L"CurrentBuildNumber", RRF_RT_REG_SZ, nullptr, buf, &cb) == ERROR_SUCCESS)
        build = buf;
    std::wstring out = product;
    if (!display.empty()) out += L" " + display;
    if (!build.empty()) out += L" build " + build;
    if (out.empty()) out = L"(unknown)";
    return out;
}

std::wstring GetTokenStateString()
{
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken))
        return L"elevated=? (OpenProcessToken failed)";
    ATL::CHandle token(rawToken);
    int elevated = 0;
    TOKEN_ELEVATION elev = {}; DWORD sz = 0;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz))
        elevated = elev.TokenIsElevated ? 1 : 0;
    int priv = 0;
    LUID luid = {};
    if (LookupPrivilegeValueW(nullptr, SE_CREATE_GLOBAL_NAME, &luid)) {
        sz = 0;
        GetTokenInformation(token, TokenPrivileges, nullptr, 0, &sz);
        std::vector<BYTE> buf(sz);
        if (sz && GetTokenInformation(token, TokenPrivileges, buf.data(), sz, &sz)) {
            auto* tp = reinterpret_cast<TOKEN_PRIVILEGES*>(buf.data());
            for (DWORD i = 0; i < tp->PrivilegeCount; ++i) {
                if (memcmp(&tp->Privileges[i].Luid, &luid, sizeof(LUID)) == 0) {
                    priv = (tp->Privileges[i].Attributes & SE_PRIVILEGE_ENABLED) ? 2 : 1;
                    break;
                }
            }
        }
    }
    wchar_t out[128];
    swprintf_s(out, L"elevated=%d SeCreateGlobalPrivilege=%d", elevated, priv);
    return out;
}

// Запуск с редиректом stdout+stderr в файл (для status/schtasks). Без попапа консоли.
bool RunCapture(const std::wstring& exe, const std::wstring& args, const std::wstring& outFile,
               DWORD timeoutMs)
{
    HANDLE out = CreateFileW(outFile.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle hOut(out);
    if (!SetHandleInformation(hOut, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT)) return false;
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = hOut;
    si.hStdError = hOut;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi))
        return false;
    ATL::CHandle hProc(pi.hProcess);
    CloseHandle(pi.hThread);
    if (WaitForSingleObject(hProc, timeoutMs) != WAIT_OBJECT_0) {
        TerminateProcess(hProc, 1);
        return false;
    }
    DWORD code = 1;
    GetExitCodeProcess(hProc, &code);
    return code == 0;
}

// Копия файла в пакет: целиком если <= maxBytes, иначе хвост (с выравниванием на \n).
// Возвращает true если скопировано; wasTruncated — был ли обрезан хвост.
bool CopyFileTail(const std::wstring& src, const std::wstring& dst, uint64_t maxBytes,
                  bool& wasTruncated)
{
    wasTruncated = false;
    HANDLE in = CreateFileW(src.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (in == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle hIn(in);
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(hIn, &sz) || sz.QuadPart <= 0) return false;
    uint64_t copyFrom = 0;
    if ((uint64_t)sz.QuadPart > maxBytes) {
        copyFrom = (uint64_t)sz.QuadPart - maxBytes;
        wasTruncated = true;
    }
    LARGE_INTEGER off = {};
    off.QuadPart = (LONGLONG)copyFrom;
    if (!SetFilePointerEx(hIn, off, nullptr, FILE_BEGIN)) return false;
    uint64_t tailSize = (uint64_t)sz.QuadPart - copyFrom;
    std::vector<char> data((size_t)tailSize);
    DWORD read = 0, total = 0;
    while (total < tailSize &&
           ReadFile(hIn, data.data() + total, (DWORD)(tailSize - total), &read, nullptr) &&
           read > 0)
        total += read;
    if (total == 0) return false;
    size_t start = 0;
    if (wasTruncated) {
        while (start < total && data[start] != '\n') start++;
        if (start < total) start++; else start = 0;
    }
    HANDLE out = CreateFileW(dst.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle hOut(out);
    DWORD w = 0;
    if (wasTruncated) {
        const char note[] = "[... truncated, showing last 200KB ...]\n";
        WriteFile(hOut, note, (DWORD)strlen(note), &w, nullptr);
    }
    if (start < total)
        WriteFile(hOut, data.data() + start, (DWORD)(total - start), &w, nullptr);
    return true;
}

} // namespace diag

// P2.3: `diag [--out <dir>] [--settings <path>]` — пакет диагностики для поддержки.
// Одна команда: версии, status+status.json, settings.json + транзенты/счётчики,
// хвосты логов (200КБ), token, задача автозапуска, список дампов → каталог-артефакт.
// Только чтение (кроме создания пакета); частичный успех — всё равно exit 0.
int CmdDiag(const std::wstring& outDirArg, const std::wstring& settingsPath)
{
    // 1. Каталог пакета (%TEMP%\VCamDiag_<ts> или --out; коллизия → суффикс).
    std::wstring bundle;
    if (!outDirArg.empty()) {
        bundle = outDirArg;
    } else {
        wchar_t temp[MAX_PATH] = {};
        DWORD tn = GetTempPathW(MAX_PATH, temp);
        if (tn == 0 || tn >= MAX_PATH) {
            LogErr(L"diag: cannot resolve TEMP");
            return 1;
        }
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        wchar_t name[64];
        swprintf_s(name, L"VCamDiag_%04u%02u%02u_%02u%02u%02u",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        bundle = std::wstring(temp) + name;
        for (int i = 2; i < 100; i++) {
            WIN32_FILE_ATTRIBUTE_DATA a = {};
            if (!GetFileAttributesExW(bundle.c_str(), GetFileExInfoStandard, &a)) break;
            swprintf_s(name, L"VCamDiag_%04u%02u%02u_%02u%02u%02u_%d",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, i);
            bundle = std::wstring(temp) + name;
        }
    }
    if (!diag::EnsureDir(bundle)) {
        LogErr(L"diag: cannot create directory %s", bundle.c_str());
        return 1;
    }
    int collected = 0, missing = 0;
    auto got = [&](bool ok) { if (ok) collected++; else missing++; };

    wchar_t self[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring selfDir(self);
    size_t sl = selfDir.find_last_of(L"\\/");
    if (sl != std::wstring::npos) selfDir.resize(sl);

    // 2. versions.txt (self/host/MediaSource same-dir + PF; OS).
    {
        std::wstring v = L"cli: " + std::wstring(self) + L"\n";
        std::wstring ver;
        if (diag::GetFileVersionString(self, ver)) v += L"  version: " + ver + L"\n";
        else v += L"  version: (unknown)\n";
        const wchar_t* pf = L"C:\\Program Files\\VCam\\";
        std::wstring hostSame = selfDir + L"\\VCamVideoStreamProducer.exe";
        std::wstring hostPf = std::wstring(pf) + L"VCamVideoStreamProducer.exe";
        std::wstring dllSame = selfDir + L"\\MediaSource.dll";
        std::wstring dllPf = std::wstring(pf) + L"MediaSource.dll";
        auto addFile = [&](const std::wstring& label, const std::wstring& p) {
            WIN32_FILE_ATTRIBUTE_DATA a = {};
            if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) {
                v += label + L": (not found) " + p + L"\n";
                return;
            }
            ULARGE_INTEGER sz = {};
            sz.LowPart = a.nFileSizeLow; sz.HighPart = a.nFileSizeHigh;
            FILETIME ft = a.ftLastWriteTime;
            SYSTEMTIME lst = {}, st = {};
            FileTimeToSystemTime(&ft, &lst);
            SystemTimeToTzSpecificLocalTime(nullptr, &lst, &st);
            wchar_t dt[64];
            swprintf_s(dt, L"%04u-%02u-%02u %02u:%02u:%02u",
                st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
            std::wstring vv;
            diag::GetFileVersionString(p, vv);
            wchar_t line[512];
            swprintf_s(line, L"%s: %s | version %s | %llu bytes | %s\n",
                label.c_str(), p.c_str(), vv.empty() ? L"(unknown)" : vv.c_str(),
                sz.QuadPart, dt);
            v += line;
        };
        addFile(L"host(same-dir)", hostSame);
        if (hostPf != hostSame) addFile(L"host(PF)", hostPf);
        addFile(L"mediasource(same-dir)", dllSame);
        if (dllPf != dllSame) addFile(L"mediasource(PF)", dllPf);
        v += L"os: " + diag::GetOsVersionString() + L"\n";
        got(diag::WriteTextFile(bundle + L"\\versions.txt", v));
    }

    // 3. status.txt + status.json (переиспользуем проверенные пути, без дублирования).
    got(diag::RunCapture(self, L"status", bundle + L"\\status.txt", 15000));
    got(diag::RunCapture(self, L"status --json", bundle + L"\\status.json", 15000));

    // 4. settings.json + мелкие state/counters (целиком).
    {
        wchar_t appdata[MAX_PATH] = {};
        DWORD an = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
        std::wstring vd = (an > 0 && an < MAX_PATH)
            ? (std::wstring(appdata) + L"\\VCam\\") : std::wstring();
        auto copyWhole = [&](const std::wstring& src, const std::wstring& dstName) {
            if (src.empty()) { missing++; return; }
            if (CopyFileW(src.c_str(), (bundle + L"\\" + dstName).c_str(), FALSE))
                collected++;
            else missing++;
        };
        copyWhole(settingsPath, L"settings.json");
        if (!vd.empty()) {
            copyWhole(vd + L"record_state.json", L"record_state.json");
            copyWhole(vd + L"hotkey_state.json", L"hotkey_state.json");
            copyWhole(vd + L"failopen_counters.json", L"failopen_counters.json");
            copyWhole(vd + L"runtime_counters.json", L"runtime_counters.json");
            copyWhole(vd + L"host_runstate.json", L"host_runstate.json");
        } else {
            missing += 5;
        }
    }

    // 5. Хвосты логов (200КБ; .old тоже, если есть).
    {
        wchar_t localApp[MAX_PATH] = {}, temp[MAX_PATH] = {}, progData[MAX_PATH] = {};
        DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH);
        DWORD tn2 = GetTempPathW(MAX_PATH, temp);
        DWORD pn = GetEnvironmentVariableW(L"ProgramData", progData, MAX_PATH);
        auto tail = [&](const std::wstring& src, const std::wstring& dstName) {
            if (src.empty()) { missing++; return; }
            bool trunc = false;
            if (diag::CopyFileTail(src, bundle + L"\\" + dstName, diag::kTailBytes, trunc))
                collected++;
            else missing++;
        };
        if (ln > 0 && ln < MAX_PATH) {
            std::wstring vd = std::wstring(localApp) + L"\\VCam\\";
            tail(vd + L"host.log", L"host.log");
            tail(vd + L"host.log.old", L"host.log.old");
        } else { missing += 2; }
        if (pn > 0 && pn < MAX_PATH) {
            std::wstring vd = std::wstring(progData) + L"\\VCam\\";
            tail(vd + L"msrc_diag.log", L"msrc_diag.shared.log");
            tail(vd + L"msrc_diag.log.old", L"msrc_diag.shared.log.old");
        } else { missing += 2; }
        if (tn2 > 0 && tn2 < MAX_PATH) {
            std::wstring vd = std::wstring(temp) + L"VCam\\";
            tail(vd + L"msrc_diag.log", L"msrc_diag.temp.log");
            tail(vd + L"msrc_diag.log.old", L"msrc_diag.temp.log.old");
        } else { missing += 2; }
    }

    // 6. token.txt (elevated + SeCreateGlobalPrivilege — важно для Local/Global).
    got(diag::WriteTextFile(bundle + L"\\token.txt", diag::GetTokenStateString() + L"\n"));

    // 7. autostart_task.txt (задача VCamHost; отсутствие — тоже сигнал).
    got(diag::RunCapture(L"schtasks.exe", L"/Query /TN VCamHost /FO LIST /V",
                         bundle + L"\\autostart_task.txt", 15000));

    // 8. crashes.txt (список дампов, НЕ сами дампы — большие; запросят отдельно).
    {
        wchar_t localApp[MAX_PATH] = {};
        DWORD ln = GetEnvironmentVariableW(L"LOCALAPPDATA", localApp, MAX_PATH);
        std::wstring t = L"crash dumps (%LOCALAPPDATA%\\VCam\\Crashes\\, content not included):\n";
        if (ln > 0 && ln < MAX_PATH) {
            std::wstring mask = std::wstring(localApp) + L"\\VCam\\Crashes\\*.dmp";
            WIN32_FIND_DATAW fd = {};
            HANDLE fh = FindFirstFileW(mask.c_str(), &fd);
            if (fh != INVALID_HANDLE_VALUE) {
                int n = 0;
                do {
                    ULARGE_INTEGER sz = {};
                    sz.LowPart = fd.nFileSizeLow; sz.HighPart = fd.nFileSizeHigh;
                    FILETIME ft = fd.ftLastWriteTime;
                    SYSTEMTIME lst = {}, st = {};
                    FileTimeToSystemTime(&ft, &lst);
                    SystemTimeToTzSpecificLocalTime(nullptr, &lst, &st);
                    wchar_t line[512];
                    swprintf_s(line, L"  %s | %llu bytes | %04u-%02u-%02u %02u:%02u:%02u\n",
                        fd.cFileName, sz.QuadPart,
                        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
                    t += line;
                    n++;
                } while (FindNextFileW(fh, &fd));
                FindClose(fh);
                if (n == 0) t += L"  (none)\n";
            } else {
                t += L"  (none)\n";
            }
        } else {
            t += L"  (LOCALAPPDATA unavailable)\n";
        }
        got(diag::WriteTextFile(bundle + L"\\crashes.txt", t));
    }

    // 9. manifest.txt (что внутри + приватность).
    {
        SYSTEMTIME st = {};
        GetLocalTime(&st);
        wchar_t ts[64];
        swprintf_s(ts, L"%04u-%02u-%02u %02u:%02u:%02u (local)",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::wstring m = L"VCam diagnostic bundle\ncreated: ";
        m += ts;
        m += L"\ncli: ";
        m += self;
        m += L"\ncontents: versions.txt, status.txt, status.json, settings.json,\n"
             L"  record_state.json, hotkey_state.json, failopen_counters.json,\n"
             L"  runtime_counters.json, host_runstate.json (each if present),\n"
             L"  host.log[.old], msrc_diag.*.log[.old] (tails, last 200KB each),\n"
             L"  token.txt, autostart_task.txt, crashes.txt (list only).\n"
             L"privacy: settings.json contains your file paths; review before sending.\n"
             L"missing files are skipped (see console summary), partial bundle still useful.\n";
        got(diag::WriteTextFile(bundle + L"\\manifest.txt", m));
    }

    Log(L"diag bundle: %s (%d collected, %d missing)", bundle.c_str(), collected, missing);
    Log(L"diag done: attach the directory (or zip it) to the issue");
    return 0;
}

} // namespace

int wmain(int argc, wchar_t* argv[])
{
    vcam::InstallCrashHandler(L"cli"); // P2.1: минидамп на AV (первым делом)
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
                   (o.setControl ? 1 : 0) + (o.diag ? 1 : 0);
    if (cmdCount == 0) {
        if (argc <= 1) {
            PrintHelp();
            return 0;
        }
        LogErr(L"error: expected command 'run', 'status', 'list-devices', "
               L"'list-controls', 'get-control', 'set-control' or 'diag'");
        PrintHelp();
        return 2;
    }
    if (cmdCount > 1) {
        LogErr(L"error: commands 'run', 'status', 'list-devices', 'list-controls', "
               L"'get-control', 'set-control' and 'diag' are mutually exclusive");
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
    if (o.json && !o.status) {
        LogErr(L"error: --json is only valid with 'status'");
        return 2;
    }
    if (o.ready && !o.status) {
        LogErr(L"error: --ready is only valid with 'status'");
        return 2;
    }
    if (o.json && o.ready) {
        LogErr(L"error: --json and --ready are mutually exclusive");
        return 2;
    }
    if (!o.outDir.empty() && !o.diag) {
        LogErr(L"error: --out is only valid with 'diag'");
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

    if (o.status) return CmdStatus(settingsPath, o.json, o.ready);
    if (o.diag) return CmdDiag(o.outDir, settingsPath);
    return CmdRun(settingsPath);
}
