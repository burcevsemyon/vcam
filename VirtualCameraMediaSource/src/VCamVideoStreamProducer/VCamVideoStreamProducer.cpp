#include <windows.h>
#include <shellapi.h>

#include <cstdarg>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "FrameWriter.h"
#include "ProducerApi.h"
#include "Settings.h"
#include "SettingsWatcher.h"
#include "SharedMemoryContract.h"

#pragma comment(lib, "shell32.lib")

namespace {

constexpr wchar_t kMutexName[] = L"VCamVideoStreamProducer.Instance";
constexpr wchar_t kStopEventName[] = L"VCamVideoStreamProducer.Stop";
constexpr wchar_t kRunValueName[] = L"VCamAutostart";
constexpr wchar_t kRunKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kTrayClass[] = L"VCamVideoStreamProducerWnd";
constexpr wchar_t kTrayTip[] = L"VCam Video Stream Producer";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT ID_STATUS = 101;
constexpr UINT ID_SETTINGS = 102;
constexpr UINT ID_PREVIEW = 103;
constexpr UINT ID_AUTOSTART = 104;
constexpr UINT ID_EXIT = 105;

constexpr DWORD kFrameMs = 33;
constexpr DWORD kSwitchWindowMs = 5000;  // окно hot-switch: FlushLast, пока новый источник не готов
constexpr DWORD kOpenRetryMs = 250;
constexpr DWORD kFallbackRetryMs = 1000;

HINSTANCE g_inst = nullptr;
HWND g_hwnd = nullptr;
HANDLE g_mutex = nullptr;
HANDLE g_stop = nullptr;
HANDLE g_dirty = nullptr;
HANDLE g_worker = nullptr;
SettingsWatcher g_watcher;
NOTIFYICONDATAW g_nid = {};

CRITICAL_SECTION g_statusCs;
std::wstring g_statusText = L"Нет сигнала (старт)";

void Log(const wchar_t* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfwprintf(stdout, fmt, ap);
    va_end(ap);
    fputwc(L'\n', stdout);
    fflush(stdout);
}

void InitLogging()
{
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    if (h && h != INVALID_HANDLE_VALUE) return;
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
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

bool ApplyAutostart(bool enabled)
{
    HKEY key = nullptr;
    LONG rc = RegCreateKeyExW(HKEY_CURRENT_USER, kRunKeyPath, 0, nullptr, 0, KEY_SET_VALUE,
                              nullptr, &key, nullptr);
    if (rc != ERROR_SUCCESS) {
        Log(L"[host] autostart registry open failed: %ld", rc);
        return false;
    }
    bool ok = true;
    if (enabled) {
        wchar_t exe[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring cmd = L"\"" + std::wstring(exe) + L"\"";
        rc = RegSetValueExW(key, kRunValueName, 0, REG_SZ, (const BYTE*)cmd.c_str(),
                            (DWORD)((cmd.size() + 1) * sizeof(wchar_t)));
        if (rc != ERROR_SUCCESS) { Log(L"[host] autostart set failed: %ld", rc); ok = false; }
    } else {
        rc = RegDeleteValueW(key, kRunValueName);
        if (rc != ERROR_SUCCESS && rc != ERROR_FILE_NOT_FOUND) {
            Log(L"[host] autostart delete failed: %ld", rc);
            ok = false;
        }
    }
    RegCloseKey(key);
    return ok;
}

// Единый источник правды — settings.autostart: при старте применяем к HKCU Run.
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
    ApplyAutostart(s.autostart);
    Log(L"[host] autostart %s (HKCU Run\\%s)", s.autostart ? L"enabled" : L"disabled", kRunValueName);
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
    s.autostart = !s.autostart;
    if (!s.Save(path)) {
        Log(L"[host] toggle autostart: save failed");
        return;
    }
    ApplyAutostart(s.autostart);
    Log(L"[host] autostart -> %s", s.autostart ? L"on" : L"off");
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
        Log(L"[host] shared memory writer ready (Global\\VCam.FrameBuffer.v1)");
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
        case ID_EXIT:
            Log(L"[host] exit requested (tray menu)");
            SetEvent(g_stop);
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

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    g_inst = hInstance;
    InitLogging();
    Log(L"[host] VCamVideoStreamProducer starting");

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
        WaitForSingleObject(g_worker, 8000);
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
    return 0;
}
