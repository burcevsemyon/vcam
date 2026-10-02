#include <windows.h>
#include <shellapi.h>
#include <tlhelp32.h>

#include <cstdarg>
#include <cstdio>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "FrameWriter.h"
#include "ProducerApi.h"
#include "Settings.h"
#include "SettingsWatcher.h"
#include "SharedMemoryContract.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

namespace {

constexpr wchar_t kMutexName[] = L"VCamVideoStreamProducer.Instance";
constexpr wchar_t kStopEventName[] = L"VCamVideoStreamProducer.Stop";
constexpr wchar_t kTrayClass[] = L"VCamVideoStreamProducerWnd";
constexpr wchar_t kTrayTip[] = L"VCam Video Stream Producer";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_STATUS = 101;
constexpr UINT ID_SETTINGS = 102;
constexpr UINT ID_PREVIEW = 103;
constexpr UINT ID_AUTOSTART = 104;
constexpr UINT ID_EXIT = 105;
constexpr UINT ID_ABOUT = 106;

constexpr DWORD kFrameMs = 33;
constexpr DWORD kSwitchWindowMs = 5000;  // окно hot-switch: FlushLast, пока новый источник не готов
constexpr DWORD kOpenRetryMs = 250;
constexpr DWORD kFallbackRetryMs = 1000;
constexpr DWORD kConsumerStaleMs = 3000; // heartbeat читателя старше → потребителя нет

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
HANDLE g_mutex = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_dirty = nullptr;
HANDLE g_worker = nullptr;
SettingsWatcher g_watcher;
NOTIFYICONDATAW g_nid = {};
HANDLE g_logFile = INVALID_HANDLE_VALUE;

CRITICAL_SECTION g_statusCs;
std::wstring g_statusText = L"Нет сигнала (старт)";

// %LOCALAPPDATA%\VCam\host.log — правда для пост-мортема после автозапуска
// из HKCU\Run (stdout туда не идёт). Ротация: >1 МБ → host.log.old.
// FILE_SHARE_READ|WRITE — лог читается живьём, пока хост его пишет.
std::wstring LogFilePath()
{
    wchar_t base[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    std::wstring dir = std::wstring(base) + L"\\VCam";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir + L"\\host.log";
}

void RotateLog(const std::wstring& path)
{
    WIN32_FILE_ATTRIBUTE_DATA a = {};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return;
    ULARGE_INTEGER sz = {};
    sz.LowPart = a.nFileSizeLow;
    sz.HighPart = a.nFileSizeHigh;
    if (sz.QuadPart <= 1024ull * 1024ull) return;
    std::wstring old = path + L".old";
    DeleteFileW(old.c_str());
    MoveFileW(path.c_str(), old.c_str()); // залочено читателем → просто не ротируем
}

void Log(const wchar_t* fmt, ...)
{
    wchar_t msg[768];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t line[860];
    swprintf_s(line, L"[%02u:%02u:%02u.%03u] %s\r\n", st.wHour, st.wMinute, st.wSecond,
               st.wMilliseconds, msg);

    fputws(line, stdout);
    fflush(stdout);
    if (g_logFile != INVALID_HANDLE_VALUE) {
        char utf8[2200];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (n > 2) {
            DWORD written = 0;
            WriteFile(g_logFile, utf8, (DWORD)(n - 1), &written, nullptr); // без '\0'
        }
    }
}

void InitLogging()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (!h || h == INVALID_HANDLE_VALUE) {
        if (AttachConsole(ATTACH_PARENT_PROCESS)) {
            FILE* f = nullptr;
            freopen_s(&f, "CONOUT$", "w", stdout);
        }
    }
    std::wstring path = LogFilePath();
    if (!path.empty()) {
        RotateLog(path);
        g_logFile = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                                FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_logFile == INVALID_HANDLE_VALUE) {
            // каталог мог не создаться/нет прав — печать в файл просто выключена
        }
    }
}

void SetStatus(const std::wstring& text)
{
    EnterCriticalSection(&g_statusCs);
    g_statusText = text;
    LeaveCriticalSection(&g_statusCs);
}

std::wstring GetStatus()
{
    EnterCriticalSection(&g_statusCs);
    std::wstring s = g_statusText;
    LeaveCriticalSection(&g_statusCs);
    return s;
}

bool PathExists(const std::wstring& p)
{
    DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring ExeDirectory()
{
    wchar_t buf[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? p : p.substr(0, slash);
}

// (a) файл рядом с exe; (b) перебор вверх по каталогам с относительным путём (rel).
std::wstring FindHelperExe(const wchar_t* fileName, const wchar_t* rel)
{
    std::wstring root = ExeDirectory();
    for (int i = 0; i < 10; i++) {
        if (i > 0) {
            size_t slash = root.find_last_of(L"\\/");
            if (slash == std::wstring::npos || slash < 3) break;
            root = root.substr(0, slash);
        }
        std::wstring direct = root + L"\\" + fileName;
        if (PathExists(direct)) return direct;
        if (rel) {
            std::wstring candidate = root + L"\\" + rel;
            if (PathExists(candidate)) return candidate;
        }
    }
    return std::wstring();
}

void LaunchHelper(const std::wstring& path)
{
    std::wstring dir = path.substr(0, path.find_last_of(L"\\/"));
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.lpVerb = L"open";
    sei.lpFile = path.c_str();
    sei.lpDirectory = dir.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) {
        Log(L"[host] launch failed: %s (%lu)", path.c_str(), GetLastError());
        return;
    }
    Log(L"[host] launched: %s", path.c_str());
}

void OpenSettingsUi()
{
    std::wstring p = FindHelperExe(
        L"VCamSettingsUi.exe",
        L"src\\VCamSettingsUi\\bin\\x64\\Release\\net10.0-windows\\VCamSettingsUi.exe");
    if (p.empty()) {
        Log(L"[host] VCamSettingsUi.exe not found");
        MessageBoxW(g_hwnd, L"VCamSettingsUi.exe не найден рядом с хостом и в исходниках репозитория.",
                    L"VCam", MB_OK | MB_ICONWARNING);
        return;
    }
    LaunchHelper(p);
}

void OpenPreview()
{
    std::wstring p = FindHelperExe(L"VCamPreview.exe", L"build\\x64\\Release\\VCamPreview.exe");
    if (p.empty()) {
        Log(L"[host] VCamPreview.exe not found");
        MessageBoxW(g_hwnd, L"VCamPreview.exe не найден рядом с хостом и в папке build\\x64\\Release.",
                    L"VCam", MB_OK | MB_ICONWARNING);
        return;
    }
    LaunchHelper(p);
}

// --- Жизненный цикл камеры: хост владеет холдером Registrar.exe.
// Старт: поднимаем `Registrar.exe add VCam hold-watch` (если его ещё нет) —
// камера registered, пока живёт хост/потребители. «Выход» из tray:
//   - есть активный потребитель (heartbeat readerLastActiveTick свежий) —
//     холдер НЕ трогаем, сессия не рвётся: писатель остановлен, и через 7 с
//     потребитель видит NO SIGNAL;
//   - потребителей нет — taskkill холдера, камера исчезает из списка устройств.
// Stop-event/краш холдера НЕ трогают: отдельный процесс переживает рестарт
// хоста; hold-watch сам выходит (без писателя и потребителей >30 с).

bool IsRegistrarRunning()
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Registrar.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Активен ли сейчас потребитель камеры: свежий heartbeat читателя (MediaSource
// пишет на каждой доставке сэмплов живой MF-сессии). Секцию открываем read-only,
// сами ничего не создаём — нет секции → нет и потребителя.
bool IsConsumerActive()
{
    const wchar_t* prefixes[] = { L"Global\\", L"Local\\" };
    const wchar_t* base = wcschr(vcam::VCamSectionName, L'\\');
    base = base ? base + 1 : vcam::VCamSectionName;
    for (const wchar_t* pre : prefixes) {
        wchar_t name[MAX_PATH] = {};
        swprintf_s(name, L"%s%s", pre, base);
        HANDLE h = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!h) continue;
        bool active = false;
        void* p = MapViewOfFile(h, FILE_MAP_READ, 0, 0, sizeof(vcam::VCamSectionHeader));
        if (p) {
            auto* hdr = static_cast<vcam::VCamSectionHeader*>(p);
            if (hdr->magic == vcam::VCamMagic) {
                ULONGLONG tick = hdr->readerLastActiveTick;
                ULONGLONG now = GetTickCount64();
                active = (tick != 0) && (now >= tick) && (now - tick) <= kConsumerStaleMs;
            }
            UnmapViewOfFile(p);
        }
        CloseHandle(h);
        if (active) return true;
    }
    return false;
}

void StartCameraHolder()
{
    if (IsRegistrarRunning()) {
        Log(L"[host] camera holder already running");
        return;
    }
    std::wstring p = FindHelperExe(L"Registrar.exe", L"build\\x64\\Release\\Registrar.exe");
    if (p.empty()) {
        Log(L"[host] Registrar.exe not found - camera will NOT be registered");
        return;
    }
    std::wstring cmd = L"\"" + p + L"\" add VCam hold-watch";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.emplace_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"[host] camera holder start failed: %s (%lu)", p.c_str(), GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess); // detached: холдер переживает смерть хоста
    Log(L"[host] camera holder started: %s", p.c_str());
}

void StopCameraHolder()
{
    if (!IsRegistrarRunning()) {
        Log(L"[host] camera holder not running - camera already gone");
        return;
    }
    // Потребитель держит сессию — камеру не трогаем (сессию не рвём): писатель
    // уже остановлен, через 7 с потребитель увидит NO SIGNAL. Холдер уйдёт сам
    // (hold-watch), когда потребитель закроется.
    if (IsConsumerActive()) {
        Log(L"[host] consumers active - camera kept (writers stopped -> NO SIGNAL in 7s)");
        return;
    }
    wchar_t sys[MAX_PATH] = {};
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring cmd = std::wstring(L"\"") + sys + L"\\taskkill.exe\" /IM Registrar.exe /F";
    std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.emplace_back(0);
    STARTUPINFOW si = {}; si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        Log(L"[host] taskkill(Registrar) failed: %lu", GetLastError());
        return;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD rc = 1;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    // taskkill падает с Access Denied, если холдер элевирован (legacy <=0.0.2
    // поднимал его инсталлер) — не врём в лог; апгрейд-инсталлер убивает
    // legacy-холдера в ssPostInstall, после чего хост поднимает свой.
    if (rc == 0 && !IsRegistrarRunning())
        Log(L"[host] camera holder stopped - camera removed from device list");
    else
        Log(L"[host] taskkill(Registrar) rc=%lu - holder may still be running (elevated?)", rc);
}

// Версия из VERSIONINFO (version.rc) — единый источник с AppVersion инсталлятора.
std::wstring ProductVersionString()
{
    wchar_t mod[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, mod, MAX_PATH)) {
        DWORD dummy = 0;
        DWORD sz = GetFileVersionInfoSizeW(mod, &dummy);
        if (sz) {
            std::vector<BYTE> buf(sz);
            if (GetFileVersionInfoW(mod, 0, sz, buf.data())) {
                VS_FIXEDFILEINFO* ffi = nullptr;
                UINT len = 0;
                if (VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&ffi), &len) && ffi) {
                    wchar_t out[64];
                    swprintf_s(out, L"%u.%u.%u", HIWORD(ffi->dwProductVersionMS),
                               LOWORD(ffi->dwProductVersionMS), HIWORD(ffi->dwProductVersionLS));
                    return out;
                }
            }
        }
    }
    return L"0.0.0";
}

void ShowAbout()
{
    std::wstring msg =
        L"VCam Virtual Camera — виртуальная камера Windows.\n"
        L"Версия " + ProductVersionString() + L"\n\n"
        L"Хост пишет кадры источника в общую память, MediaSource.dll\n"
        L"отдаёт их потребителям (Zoom, ktalk, Windows Камеры…).\n\n"
        L"Настройки и предпросмотр — двойной клик / ПКМ по иконке в трее.";
    MessageBoxW(g_hwnd, msg.c_str(), L"О программе — VCam", MB_OK | MB_ICONINFORMATION);
}

// --- Автозапуск: задача Планировщика Task Scheduler\VCamHost вместо HKCU\Run.
// Run-ключ стартовал хост с Limited-токеном (UAC) → Create(Global) не проходил,
// писатель уходил в Local-секцию, а сервис MF видит только Global → раскол
// «после ребута нет сигнала». Задача /RL HIGHEST даёт полный токен при входе
// → хост сразу в Global, один мир. ONLOGON-задачу создать/удалить можно только
// с правами (из Limited — через runas-посредник с UAC); /Query доступен всем.

constexpr wchar_t kAutostartTask[] = L"VCamHost";

// Прогон schtasks, скрыто; elevate=true — через runas (UAC). Возврат: exit code
// schtasks, 1223 = пользователь отказался от UAC, -1 = не запустилось.
int RunSchtasks(const std::wstring& args, bool elevate)
{
    wchar_t sys[MAX_PATH] = {};
    if (GetSystemDirectoryW(sys, MAX_PATH) == 0) return -1;
    std::wstring exe = std::wstring(sys) + L"\\schtasks.exe";
    if (!elevate) {
        std::wstring cmd = L"\"" + exe + L"\" " + args;
        std::vector<wchar_t> buf(cmd.begin(), cmd.end()); buf.emplace_back(0);
        STARTUPINFOW si = {}; si.cb = sizeof(si);
        si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
            return -1;
        CloseHandle(pi.hThread);
        WaitForSingleObject(pi.hProcess, 15000);
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        return (int)code;
    }
    SHELLEXECUTEINFOW sei = {};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&sei))
        return GetLastError() == ERROR_CANCELLED ? 1223 : -1;
    if (sei.hProcess) {
        WaitForSingleObject(sei.hProcess, 30000);
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(sei.hProcess, &code);
        CloseHandle(sei.hProcess);
        return (int)code;
    }
    return 0;
}

std::wstring AutostartTaskArgs(bool enabled)
{
    if (!enabled)
        return std::wstring(L"/Delete /TN ") + kAutostartTask + L" /F";
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    // /TR с экранированными кавычками — путь Program Files содержит пробелы
    return std::wstring(L"/Create /TN ") + kAutostartTask + L" /TR \"\\\"" +
           exe + L"\\\"\" /SC ONLOGON /RL HIGHEST /F";
}

bool IsAutostartTaskPresent()
{
    return RunSchtasks(std::wstring(L"/Query /TN ") + kAutostartTask, false) == 0;
}

// Применение желаемого состояния: сверка бесплатна, изменение — только через
// подходящий токен (elevated-хост) либо runas с UAC из Limited.
bool ApplyAutostart(bool enabled)
{
    if (IsAutostartTaskPresent() == enabled) return true;
    int rc = RunSchtasks(AutostartTaskArgs(enabled), false);
    if (rc != 0) rc = RunSchtasks(AutostartTaskArgs(enabled), true);
    if (rc != 0) {
        Log(L"[host] autostart task apply failed (enabled=%d, rc=%d)", (int)enabled, rc);
        return false;
    }
    return true;
}

// Стартовый ход: только сверка и лог — UAC-диалог при входе недопустим.
// Задачу при установке создаёт инсталлятор (admin); этот ход лишь фиксирует
// расхождение, чтобы не молчать.
void ApplyAutostartFromSettings()
{
    std::wstring path = DefaultSettingsPath();
    if (path.empty()) { Log(L"[host] settings path is empty - autostart skipped"); return; }
    Settings s;
    bool loaded = s.Load(path);
    if (!loaded && PathExists(path)) {
        Log(L"[host] settings unreadable - autostart left untouched");
        return;
    }
    bool present = IsAutostartTaskPresent();
    if (present == s.autostart) {
        Log(L"[host] autostart %s (Task Scheduler\\%s)",
            s.autostart ? L"enabled" : L"disabled", kAutostartTask);
    } else {
        Log(L"[host] autostart mismatch (settings=%d, task=%s) - fix via tray menu",
            (int)s.autostart, present ? L"present" : L"absent");
    }
}

void ToggleAutostart()
{
    std::wstring path = DefaultSettingsPath();
    if (path.empty()) {
        MessageBoxW(g_hwnd, L"Не найден путь к settings.json (APPDATA).", L"VCam",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    Settings s;
    bool loaded = s.Load(path);
    if (!loaded && PathExists(path)) {
        Log(L"[host] toggle autostart: settings unreadable");
        MessageBoxW(g_hwnd, L"Не удалось прочитать settings.json — автозагрузка не изменена.", L"VCam",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    bool want = !s.autostart;
    int rc = RunSchtasks(AutostartTaskArgs(want), false);
    if (rc != 0) {
        rc = RunSchtasks(AutostartTaskArgs(want), true);
        if (rc == 1223) { Log(L"[host] autostart toggle: UAC declined"); return; }
    }
    if (rc != 0) {
        Log(L"[host] toggle autostart failed (rc=%d)", rc);
        MessageBoxW(g_hwnd, L"Не удалось изменить задачу автозапуска (Task Scheduler).", L"VCam",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    s.autostart = want;
    if (!s.Save(path)) {
        Log(L"[host] toggle autostart: settings save failed (task already changed)");
        return;
    }
    Log(L"[host] autostart -> %s", want ? L"on" : L"off");
}

enum class Phase { Switch, Active, Fallback };

// Метка источника для статуса/логов: для camera — имя устройства (symlink
// слишком длинный для трей-меню), для остальных — путь.
std::wstring TargetLabel(const SourceConfig& cfg)
{
    if (!cfg.camName.empty()) return cfg.camName;
    if (cfg.path.empty()) return cfg.type == L"camera" ? std::wstring(L"(камера не выбрана)")
                                                       : std::wstring(L"(путь не задан)");
    return cfg.path;
}

std::wstring ActiveStatus(const SourceConfig& cfg)
{
    return L"Источник: " + cfg.type + L" — " + TargetLabel(cfg);
}

std::wstring FallbackStatus(const std::wstring& reason)
{
    return L"Нет сигнала (" + reason + L")";
}

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
    SetStatus(FallbackStatus(reason));
    Log(L"[host] no signal: %s", reason.c_str());
}

void SetActiveStatus(Machine& m)
{
    if (m.writerOpen) SetStatus(ActiveStatus(m.target));
    else SetStatus(FallbackStatus(m.writerErr));
}

void BeginSwitch(Machine& m, const SourceConfig& want)
{
    std::wstring wantLabel = TargetLabel(want);
    Log(L"[host] switch: type=%s path=%s (was type=%s path=%s)",
        want.type.c_str(), wantLabel.c_str(),
        m.hasTarget ? m.target.type.c_str() : L"-",
        m.hasTarget ? TargetLabel(m.target).c_str() : L"-");
    CloseSource(m);
    m.target = want;
    m.hasTarget = true;
    m.phase = Phase::Switch;
    m.switchStart = GetTickCount64();
    m.nextAttempt = m.switchStart;
}

// Шаг state machine. Возвращает, сколько мс можно ждать до следующего шага.
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
                EnterFallback(m, L"неизвестный тип источника: " + m.target.type);
                return 0;
            }
            std::wstring err;
            if (cand->Open(m.target, err)) {
                m.src = std::move(cand);
                Log(L"[host] source opened: type=%s path=%s",
                    m.target.type.c_str(), TargetLabel(m.target).c_str());
            } else {
                m.nextAttempt = now + kOpenRetryMs;
                Log(L"[host] open failed: %s", err.c_str());
                if (now - m.switchStart >= kSwitchWindowMs) {
                    EnterFallback(m, L"не удалось открыть: " + err);
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
            SetActiveStatus(m);
            Log(L"[host] active: type=%s path=%s%s",
                m.target.type.c_str(), TargetLabel(m.target).c_str(),
                written ? L"" : L" (write failed)");
            return 0;
        }
        if (now - m.switchStart >= kSwitchWindowMs) {
            EnterFallback(m, L"источник не даёт кадры: " + (rerr.empty() ? L"?" : rerr));
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
        EnterFallback(m, rerr.empty() ? L"источник не даёт кадры" : rerr);
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
                        SetActiveStatus(m);
                        Log(L"[host] signal restored: type=%s path=%s",
                            m.target.type.c_str(), TargetLabel(m.target).c_str());
                        return 0;
                    }
                } else {
                    Log(L"[host] retry open failed: %s", err.c_str());
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

DWORD WINAPI WorkerProc(LPVOID)
{
    Machine m;
    m.buf.resize(vcam::VCamFrameSize);

    std::wstring err;
    m.writerOpen = m.writer.Open(err);
    if (m.writerOpen) {
        Log(L"[host] shared memory writer ready (%s)", m.writer.SectionOpenedAs().c_str());
    } else {
        m.writerErr = err.empty() ? L"writer open failed" : err;
        Log(L"[host] writer open failed: %s", err.c_str());
        SetStatus(FallbackStatus(m.writerErr));
    }

    HANDLE waits[2] = { g_stop, g_dirty };
    bool first = true;
    DWORD timeout = 0;
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (r == WAIT_OBJECT_0) break;
        if (first || r == WAIT_OBJECT_0 + 1) {
            first = false;
            Settings s;
            g_watcher.Current(s);
            SourceConfig want = ToSourceConfig(s);
            if (!m.hasTarget || want != m.target) BeginSwitch(m, want);
        }
        timeout = Step(m);
    }

    CloseSource(m);
    m.writer.Close();
    Log(L"[host] worker stopped");
    return 0;
}

void OnSettingsChanged(const Settings&) { SetEvent(g_dirty); }

void ShowTrayMenu(HWND hwnd)
{
    POINT pt = {};
    GetCursorPos(&pt);

    Settings s;
    s.Load(DefaultSettingsPath()); // при неудаче остаются default (autostart=true)

    std::wstring status = GetStatus();
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | MF_GRAYED | MF_DISABLED, ID_STATUS, status.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_SETTINGS, L"Настройки VCam…");
    AppendMenuW(menu, MF_STRING, ID_PREVIEW, L"Окно предпросмотра…");
        AppendMenuW(menu, MF_STRING | (s.autostart ? MF_CHECKED : 0), ID_AUTOSTART, L"Автозагрузка");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_ABOUT, L"О программе…");
        AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(menu, MF_STRING, ID_EXIT, L"Выход");

    SetForegroundWindow(hwnd);
    UINT cmd = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, nullptr);
    DestroyMenu(menu);
    PostMessageW(hwnd, WM_NULL, 0, 0);
    if (cmd) SendMessageW(hwnd, WM_COMMAND, cmd, 0);
}

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_TRAYICON:
        if (lp == WM_RBUTTONUP) ShowTrayMenu(hwnd);
        else if (lp == WM_LBUTTONDBLCLK) OpenSettingsUi();
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_SETTINGS: OpenSettingsUi(); break;
        case ID_PREVIEW: OpenPreview(); break;
        case ID_AUTOSTART: ToggleAutostart(); break;
        case ID_ABOUT: ShowAbout(); break;
        case ID_EXIT:
            Log(L"[host] exit requested (tray menu)");
            SetEvent(g_stop);
            StopCameraHolder();
            break;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// Диагностика автозапуска: из HKCU\Run хост стартовал Limited (elevated=0,
// без SeCreateGlobalPrivilege) и уходил в Local-секцию; от задачи с /RL HIGHEST
// ожидается elevated=1 + включённая привилегия.
void LogTokenState()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        Log(L"[host] token: OpenProcessToken failed: %lu", GetLastError());
        return;
    }
    int elevated = 0;
    TOKEN_ELEVATION elev = {};
    DWORD sz = 0;
    if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &sz))
        elevated = elev.TokenIsElevated ? 1 : 0;

    int priv = 0; // 0 = отсутствует, 1 = есть, 2 = включена
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
    CloseHandle(token);
    Log(L"[host] token: elevated=%d SeCreateGlobalPrivilege=%d", elevated, priv);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    g_inst = hInstance;
    InitLogging();
    Log(L"[host] VCamVideoStreamProducer starting");
    LogTokenState();

    SetLastError(ERROR_SUCCESS);
    g_mutex = CreateMutexW(nullptr, FALSE, kMutexName);
    if (!g_mutex) {
        Log(L"[host] CreateMutex failed: %lu", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"[host] already running - second instance exits");
        MessageBoxW(nullptr,
                    L"Хост VCam уже запущен (VCamVideoStreamProducer.exe).\n"
                    L"Используйте значок в трее рядом с часами.",
                    L"VCam Video Stream Producer", MB_OK | MB_ICONINFORMATION);
        CloseHandle(g_mutex);
        g_mutex = nullptr;
        return 0;
    }
    WaitForSingleObject(g_mutex, INFINITE); // владение мьютексом = «хост запущен»

    g_stop = CreateEventW(nullptr, TRUE, FALSE, kStopEventName);
    if (!g_stop) {
        Log(L"[host] CreateEvent(Stop) failed: %lu", GetLastError());
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) ResetEvent(g_stop);
    g_dirty = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    InitializeCriticalSection(&g_statusCs);

    ApplyAutostartFromSettings();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc)) {
        Log(L"[host] RegisterClassEx failed: %lu", GetLastError());
    }
    g_hwnd = CreateWindowExW(0, kTrayClass, L"VCamVideoStreamProducer", WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (!g_hwnd) {
        Log(L"[host] CreateWindowEx failed: %lu", GetLastError());
        DeleteCriticalSection(&g_statusCs);
        CloseHandle(g_dirty);
        CloseHandle(g_stop);
        ReleaseMutex(g_mutex);
        CloseHandle(g_mutex);
        return 1;
    }

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wcsncpy_s(g_nid.szTip, kTrayTip, _TRUNCATE);
    if (!Shell_NotifyIconW(NIM_ADD, &g_nid)) Log(L"[host] Shell_NotifyIcon(add) failed");
    Log(L"[host] tray icon added");

    StartCameraHolder();

    std::wstring settingsPath = DefaultSettingsPath();
    if (!g_watcher.Start(settingsPath, OnSettingsChanged)) {
        Log(L"[host] settings watcher not started (settings path is empty?)");
    } else {
        Log(L"[host] watching: %s", settingsPath.c_str());
    }

    g_worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (!g_worker) Log(L"[host] CreateThread(worker) failed: %lu", GetLastError());

    for (;;) {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        DWORD r = MsgWaitForMultipleObjects(1, &g_stop, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
    }

    Log(L"[host] shutting down");
    SetEvent(g_stop);
    g_watcher.Stop();
    if (g_worker) {
        // Handles/critical sections below are shared with the worker: it must
        // be joined before they are destroyed (checked wait, not best-effort).
        if (WaitForSingleObject(g_worker, 8000) != WAIT_OBJECT_0) {
            Log(L"[host] worker did not stop in 8000 ms; waiting indefinitely");
            WaitForSingleObject(g_worker, INFINITE);
        }
        CloseHandle(g_worker);
        g_worker = nullptr;
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    Log(L"[host] tray icon removed");
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;

    CloseHandle(g_dirty);
    g_dirty = nullptr;
    CloseHandle(g_stop);
    g_stop = nullptr;
    DeleteCriticalSection(&g_statusCs);
    ReleaseMutex(g_mutex);
    CloseHandle(g_mutex);
    g_mutex = nullptr;
    Log(L"[host] exit");
    if (g_logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(g_logFile);
        g_logFile = INVALID_HANDLE_VALUE;
    }
    return 0;
}
