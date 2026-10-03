#include <windows.h>
#include <shellapi.h>
#include <shlobj.h> // B7: SHGetKnownFolderPath(FOLDERID_Videos)
#include <tlhelp32.h>
#include <atlbase.h>

#include <cctype> // B7: граница токена true/false в TryReadRecordState
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <cwchar>
#include <memory>
#include <string>
#include <vector>

#include "FrameWriter.h"
#include "Mp4Recorder.h"
#include "ProducerApi.h"
#include "Settings.h"
#include "SettingsWatcher.h"
#include "GpuEffects.h"
#include "SharedMemoryContract.h"
#include "MappedViewOfFilePtr.h"

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "version.lib")

namespace {

constexpr wchar_t kMutexName[] = L"VCamVideoStreamProducer.Instance";
constexpr wchar_t kStopEventName[] = L"VCamVideoStreamProducer.Stop";
constexpr wchar_t kTrayClass[] = L"VCamVideoStreamProducerWnd";
constexpr wchar_t kTrayTip[] = L"VCam Video Stream Producer";

constexpr UINT WM_TRAYICON = WM_APP + 1;
constexpr UINT WM_REAPPLY_HOTKEY = WM_APP + 2; // worker заметил смену hotkey в settings
constexpr UINT kHotkeyId = 1;
constexpr UINT kRecHotkeyId = 2; // старт/стоп записи эфира
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
// Единый стиль: владение хендлами — ATL::CHandle (были raw HANDLE +
// ручные CloseHandle). Сравнение через static_cast<HANDLE>, закрытие — .Close().
ATL::CHandle g_mutex;
ATL::CHandle g_stop;
ATL::CHandle g_dirty;
ATL::CHandle g_worker;
SettingsWatcher g_watcher;
NOTIFYICONDATAW g_nid = {};
ATL::CHandle g_logFile;

CRITICAL_SECTION g_statusCs;
std::wstring g_statusText = L"Нет сигнала (старт)";

void Log(const wchar_t* fmt, ...); // определён ниже (host.log + stdout)

// --- Глобальный хоткей static→video→auto-static (секция hotkey в settings).
// Нажатие (вне video) = запомнить текущий тип и уйти в video на один проход
// (playOnce через settings.json — хост и UI остаются согласованы через watcher);
// нажатие во время заимствованного video = досрочный возврат; конец файла =
// авто-возврат на запомненное (обычно static). Borrow живёт только в памяти
// хоста; для UI-индикатора пишется transient-файл hotkey_state.json
// (%APPDATA%\VCam, НЕ settings — иначе вотчер зациклит переключения).
CRITICAL_SECTION g_hotkeyCs;
HotkeySection g_hotkey; // последний зарегистрированный (дефолт = Ctrl+Alt+V)
std::wstring g_hotkeyReturnType = L"static"; // куда вернуться после borrowed video
bool g_hotkeyBorrowed = false;               // video сейчас заимствовано хоткеем
// B5: backstop-таймаут borrow — Ended не fires при битом файле, а fallback
// теперь тоже возвращает (ниже); тик страхует от вечного «идёт видео» при
// залипшем источнике, который не падает и не кончается.
ULONGLONG g_hotkeyBorrowTickMs = 0; // GetTickCount64 в момент borrow, 0 = нет borrow
constexpr ULONGLONG kBorrowMaxMs = 30ULL * 60 * 1000; // 30 мин одного borrow достаточно

// B6: сериализация всех мутаций settings.json внутри хоста: main-поток
// (хоткей WM_HOTKEY, трей ToggleAutostart) vs worker (StartRecording,
// auto-return). Без неё Load→Save из двух потоков теряли обновления друг
// друга. UI-гонка (внешний процесс) остаётся last-writer-wins — снапшоты
// полные, приемлемо; рваные чтения закрыты атомарным Save (tmp+move).
CRITICAL_SECTION g_settingsCs;
struct SettingsFileGuard {
    SettingsFileGuard() { EnterCriticalSection(&g_settingsCs); }
    ~SettingsFileGuard() { LeaveCriticalSection(&g_settingsCs); }
};

std::wstring HotkeyStatePath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appdata) + L"\\VCam\\hotkey_state.json";
}

// Человекочитаемая комбинация для логов/UI ("Ctrl+Alt+V"). Неизвестный vk —
// "VK 0x..". Шаблон: HotkeySection и RecordHotkeySection (одинаковые поля).
template <typename T>
std::wstring HotkeyDisplay(const T& hk)
{
    std::wstring s;
    if (hk.modifiers & MOD_CONTROL) s += L"Ctrl+";
    if (hk.modifiers & MOD_ALT) s += L"Alt+";
    if (hk.modifiers & MOD_SHIFT) s += L"Shift+";
    if (hk.modifiers & MOD_WIN) s += L"Win+";
    wchar_t key[32] = {};
    if ((hk.vk >= '0' && hk.vk <= '9') || (hk.vk >= 'A' && hk.vk <= 'Z')) {
        swprintf_s(key, L"%c", (wchar_t)hk.vk);
    } else if (hk.vk >= VK_F1 && hk.vk <= VK_F24) {
        swprintf_s(key, L"F%d", hk.vk - VK_F1 + 1);
    } else {
        switch (hk.vk) {
        case VK_SPACE: wcscpy_s(key, L"Space"); break;
        case VK_RETURN: wcscpy_s(key, L"Enter"); break;
        case VK_TAB: wcscpy_s(key, L"Tab"); break;
        case VK_ESCAPE: wcscpy_s(key, L"Esc"); break;
        case VK_LEFT: wcscpy_s(key, L"Left"); break;
        case VK_RIGHT: wcscpy_s(key, L"Right"); break;
        case VK_UP: wcscpy_s(key, L"Up"); break;
        case VK_DOWN: wcscpy_s(key, L"Down"); break;
        default: swprintf_s(key, L"VK 0x%02X", (unsigned)hk.vk); break;
        }
    }
    s += key;
    return s;
}

void WriteHotkeyState(const std::wstring& returnTo)
{
    std::wstring path = HotkeyStatePath();
    if (path.empty()) return;
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(path.substr(0, slash).c_str(), nullptr);
    std::string utf8 = "{\"borrowed\":true,\"returnTo\":\"";
    for (wchar_t c : returnTo) utf8 += (c < 0x80) ? (char)c : '?'; // типы — ascii
    utf8 += "\"}\n";
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        Log(L"[host] hotkey state write failed: %lu", GetLastError());
        return;
    }
    ATL::CHandle h(raw);
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

void ClearHotkeyState()
{
    std::wstring path = HotkeyStatePath();
    if (!path.empty()) DeleteFileW(path.c_str()); // нет файла — норма
}

// --- Запись эфира в .mp4 (секции record {path} + recordHotkey в settings).
// Кадры — v1-кэш FrameWriter ПОСЛЕ ApplyFx (с эффектами), 720p RGB32→H.264.
// Управление: UI пишет record_command.json (старт/стоп), хост пишет
// record_state.json (идёт/путь/старт); оба transient в %APPDATA%\VCam.
// Рекордер трогает ТОЛЬКО worker-поток (команда хоткея с main-потока идёт
// через тот же command-файл — межпоточности нет).
// g_hotkeyCs охраняет и g_recHotkey (состояние регистрации хоткеев).
RecordHotkeySection g_recHotkey; // последний зарегистрированный (дефолт Ctrl+Alt+R)

std::wstring RecordStatePath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appdata) + L"\\VCam\\record_state.json";
}

std::wstring RecordCommandPath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return {};
    return std::wstring(appdata) + L"\\VCam\\record_command.json";
}

// -- BEGIN FIXMAJ-BLOCK-RECORDPATH (мини-тест вырезает до END FIXMAJ-BLOCK-RECORDPATH) --
// Путь записи по умолчанию: <Videos>\VCam_ГГГГММДД_ЧЧММСС.mp4.
// B7: единый резолв через SHGetKnownFolderPath(FOLDERID_Videos) — то же, что
// C# Environment.SpecialFolder.MyVideos в UI. %USERPROFILE%\Videos может не
// совпадать с перенаправленной папкой; fallback — та же цепочка, что в UI.
std::wstring DefaultRecordPath()
{
    std::wstring videos;
    wchar_t* known = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &known)) && known) {
        videos = known;
        CoTaskMemFree(known);
    }
    if (videos.empty()) {
        wchar_t up[MAX_PATH] = {};
        DWORD n = GetEnvironmentVariableW(L"USERPROFILE", up, MAX_PATH);
        if (n == 0 || n >= MAX_PATH) return {};
        videos = std::wstring(up) + L"\\Videos";
    }
    SYSTEMTIME st = {};
    GetLocalTime(&st);
    wchar_t name[64];
    swprintf_s(name, L"VCam_%04u%02u%02u_%02u%02u%02u.mp4",
               st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return videos + L"\\" + name;
}
// -- END FIXMAJ-BLOCK-RECORDPATH --

// wide → UTF-8 + экранирование для JSON-строки.
std::string EscapeJsonUtf8(const std::wstring& w)
{
    std::string out;
    if (w.empty()) return out;
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0,
                                nullptr, nullptr);
    if (n <= 0) return out;
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    for (char c : s) {
        switch (c) {
        case '\\': out += "\\\\"; break;
        case '"': out += "\\\""; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if ((unsigned char)c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", (unsigned char)c);
                out += b;
            } else {
                out += c;
            }
        }
    }
    return out;
}

void WriteRecordState(const std::wstring& path)
{
    std::wstring sp = RecordStatePath();
    if (sp.empty()) return;
    size_t slash = sp.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(sp.substr(0, slash).c_str(), nullptr);
    std::string utf8 = "{\"recording\":true,\"path\":\"";
    utf8 += EscapeJsonUtf8(path);
    utf8 += "\",\"started\":";
    utf8 += std::to_string((long long)time(nullptr));
    utf8 += "}\n";
    HANDLE raw = CreateFileW(sp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) {
        Log(L"[host] record state write failed: %lu", GetLastError());
        return;
    }
    ATL::CHandle h(raw);
    DWORD written = 0;
    WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr);
}

void ClearRecordState()
{
    std::wstring sp = RecordStatePath();
    if (!sp.empty()) DeleteFileW(sp.c_str()); // нет файла — норма
}

// -- BEGIN FIXMAJ-BLOCK-JSON (мини-тест вырезает до END FIXMAJ-BLOCK-JSON) --
// Плоский скан JSON-кавыченной строки после ключа
// (с \" \\ \/ \b \f \n \r \t \uXXXX). Возвращает false если ключа/строки нет.
// B7: строгая позиция ключа — "key" за которым (через ws) идёт ':'
// (как FindKeyPos в Settings.cpp): плоский find ловил совпадения внутри
// значений ("recordingX" для ключа "recording", "true" внутри path).
static void SkipScanWs(const std::string& json, size_t& p)
{
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' ||
                               json[p] == '\r' || json[p] == '\n'))
        p++;
}

// B7: строгая позиция ключа (как FindKeyPos в Settings.cpp).
static size_t ScanKeyPos(const std::string& json, const char* key, size_t from = 0)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k, from);
    while (p != std::string::npos) {
        size_t c = p + k.size();
        SkipScanWs(json, c);
        if (c < json.size() && json[c] == ':') return p;
        p = json.find(k, p + 1);
    }
    return std::string::npos;
}

bool ScanJsonString(const std::string& json, const char* key, std::wstring& value)
{
    size_t kp = ScanKeyPos(json, key);
    if (kp == std::string::npos) return false;
    size_t p = json.find(':', kp);
    if (p == std::string::npos) return false;
    p++;
    SkipScanWs(json, p);
    if (p >= json.size() || json[p] != '"') return false;
    std::string val;
    for (p++; p < json.size() && json[p] != '"'; p++) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            char c = json[++p];
            switch (c) {
            case '"': val += '"'; break;
            case '\\': val += '\\'; break;
            case '/': val += '/'; break; // B7: был default, явно как в JSON
            case 'b': val += '\b'; break; // B7: не хватало
            case 'f': val += '\f'; break; // B7: не хватало
            case 'n': val += '\n'; break;
            case 'r': val += '\r'; break;
            case 't': val += '\t'; break;
            case 'u':
                if (p + 4 < json.size()) p += 4;
                val += '?';
                break;
            default: val += c; break;
            }
        } else {
            val += json[p];
        }
    }
    if (p >= json.size()) return false;
    if (val.empty()) { value.clear(); return true; }
    int m = MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), nullptr, 0);
    if (m <= 0) return false;
    value.resize((size_t)m);
    MultiByteToWideChar(CP_UTF8, 0, val.c_str(), (int)val.size(), &value[0], m);
    return true;
}

bool ScanJsonInt(const std::string& json, const char* key, long long& value)
{
    size_t kp = ScanKeyPos(json, key);
    if (kp == std::string::npos) return false;
    size_t p = json.find(':', kp);
    if (p == std::string::npos) return false;
    p++;
    SkipScanWs(json, p);
    if (p >= json.size() || json[p] < '0' || json[p] > '9') return false;
    long long v = 0;
    for (; p < json.size() && json[p] >= '0' && json[p] <= '9'; p++) {
        v = v * 10 + (json[p] - '0');
        if (v > 99999999999LL) break;
    }
    value = v;
    return true;
}

bool ReadSmallFile(const std::wstring& path, std::string& out)
{
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
                             FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                             nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    LARGE_INTEGER sz = {};
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 65536) return false;
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    return ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr) && read == out.size();
}

// Идёт ли сейчас запись (для UI-индикатора, хоткея и трей-меню).
bool TryReadRecordState(std::wstring& path, long long& started)
{
    path.clear();
    started = 0;
    std::wstring sp = RecordStatePath();
    if (sp.empty()) return false;
    std::string json;
    if (!ReadSmallFile(sp, json)) return false;
    // B7: строгий bool по ключу. Было: первый ':' файла + find("true") где
    // угодно — путь "C:\true\x.mp4" при recording:false давал ложное true,
    // а "recordingX":true перекрывал настоящий ключ. Теперь — позиция ключа
    // (ScanKeyPos) + точный токен true/false с границей.
    size_t kp = ScanKeyPos(json, "recording");
    if (kp == std::string::npos) return false;
    size_t p = json.find(':', kp);
    if (p == std::string::npos) return false;
    p++;
    SkipScanWs(json, p);
    bool rec;
    if (json.compare(p, 4, "true") == 0 &&
        (p + 4 >= json.size() || (!isalnum((unsigned char)json[p + 4]) && json[p + 4] != '_')))
        rec = true;
    else if (json.compare(p, 5, "false") == 0 &&
             (p + 5 >= json.size() || (!isalnum((unsigned char)json[p + 5]) && json[p + 5] != '_')))
        rec = false;
    else
        return false;
    if (!rec) return false;
    ScanJsonString(json, "path", path);
    ScanJsonInt(json, "started", started);
    return true;
}
// -- END FIXMAJ-BLOCK-JSON --

// Команда UI/хоткея → worker: {"cmd":"start","path":"..."} / {"cmd":"stop"}.
// Писатель — UI и main-поток хоткея; атомарно через tmp+move.
bool WriteRecordCommand(const std::wstring& cmd, const std::wstring& path)
{
    std::wstring cp = RecordCommandPath();
    if (cp.empty()) return false;
    size_t slash = cp.find_last_of(L"\\/");
    if (slash != std::wstring::npos) CreateDirectoryW(cp.substr(0, slash).c_str(), nullptr);
    std::string utf8 = "{\"cmd\":\"";
    for (wchar_t c : cmd) utf8 += (c < 0x80) ? (char)c : '?'; // start|stop — ascii
    utf8 += "\",\"path\":\"" + EscapeJsonUtf8(path) + "\"}\n";
    std::wstring tmp = cp + L".tmp";
    HANDLE raw = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                             CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    {
        ATL::CHandle h(raw);
        DWORD written = 0;
        if (!WriteFile(h, utf8.data(), (DWORD)utf8.size(), &written, nullptr) ||
            written != utf8.size())
            return false;
    }
    return MoveFileWithProgressW(tmp.c_str(), cp.c_str(), nullptr, nullptr,
                                 MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

// Читает и удаляет команду (только worker). true = команда была.
// К2: атомарный захват через rename-в-обработку (cp -> *.processing +
// чтение оттуда): команда, записанная UI между нашим чтением и удалением,
// больше не теряется (старый read-then-delete её молча сносил).
// Писатель кладёт команду через tmp+move, поэтому всё, что пришло после
// нашего MoveFile, остаётся в cp до следующего опроса.
bool ConsumeRecordCommand(std::wstring& cmd, std::wstring& path)
{
    cmd.clear();
    path.clear();
    std::wstring cp = RecordCommandPath();
    if (cp.empty()) return false;
    std::wstring proc = cp + L".processing";
    // Нет файла — команды нет. REPLACE_EXISTING заодно подбирает зависший
    // .processing от краша между move и delete.
    if (!MoveFileWithProgressW(cp.c_str(), proc.c_str(), nullptr, nullptr,
                               MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return false;
    std::string json;
    if (!ReadSmallFile(proc, json)) {
        DeleteFileW(proc.c_str());
        return false;
    }
    DeleteFileW(proc.c_str()); // обработали — удаляем (повторов не будет)
    if (!ScanJsonString(json, "cmd", cmd)) return false;
    ScanJsonString(json, "path", path); // у stop пути может не быть — норма
    return !cmd.empty();
}

// (Пере)регистрация глобальных хоткеев на окне трея. Вызывать только из
// main-потока (владелец g_hwnd): из worker — PostMessage WM_REAPPLY_HOTKEY.
void ApplyHotkeyRegistration()
{
    if (!g_hwnd) return;
    UnregisterHotKey(g_hwnd, kHotkeyId); // не было — норма
    UnregisterHotKey(g_hwnd, kRecHotkeyId);
    HotkeySection hk;
    RecordHotkeySection rk;
    EnterCriticalSection(&g_hotkeyCs);
    hk = g_hotkey;
    rk = g_recHotkey;
    LeaveCriticalSection(&g_hotkeyCs);
    if (RegisterHotKey(g_hwnd, kHotkeyId, (UINT)hk.modifiers, (UINT)hk.vk)) {
        Log(L"[host] hotkey registered: %s", HotkeyDisplay(hk).c_str());
    } else {
        Log(L"[host] hotkey RegisterHotKey(%s) failed: %lu - hotkey disabled until settings change",
            HotkeyDisplay(hk).c_str(), GetLastError());
    }
    if (RegisterHotKey(g_hwnd, kRecHotkeyId, (UINT)rk.modifiers, (UINT)rk.vk)) {
        Log(L"[host] record hotkey registered: %s", HotkeyDisplay(rk).c_str());
    } else {
        Log(L"[host] record hotkey RegisterHotKey(%s) failed: %lu - record hotkey disabled until settings change",
            HotkeyDisplay(rk).c_str(), GetLastError());
    }
}

// B5: единый автовозврат заимствованного video (конец ролика, fallback при
// битом файле, backstop-таймаут) через settings.json — watcher подхватит как
// обычное переключение. Семантика Load-неудач как раньше: settings биты =
// retry позже (borrow жив), уже ушли вручную = просто снять borrow.
// Возвращает true если borrow снят (или его не было). Вызывать БЕЗ g_hotkeyCs.
bool AutoReturnBorrowedVideo(const std::wstring& settingsPath, const wchar_t* why)
{
    EnterCriticalSection(&g_hotkeyCs);
    bool borrowed = g_hotkeyBorrowed;
    std::wstring ret = g_hotkeyReturnType;
    LeaveCriticalSection(&g_hotkeyCs);
    if (!borrowed) return true;
    if (ret.empty()) ret = L"static";
    Settings back;
    bool loaded = !settingsPath.empty() && back.Load(settingsPath);
    if (!loaded) {
        Log(L"[host] hotkey: %s, settings unreadable - retry later", why);
        return false;
    }
    if (back.sourceType != L"video") {
        // Уже ушли вручную: просто снять borrow.
        EnterCriticalSection(&g_hotkeyCs);
        g_hotkeyBorrowed = false;
        g_hotkeyBorrowTickMs = 0;
        LeaveCriticalSection(&g_hotkeyCs);
        ClearHotkeyState();
        return true;
    }
    back.sourceType = ret;
    bool saved;
    {
        SettingsFileGuard fsg; // B6: vs StartRecording/concurrent main
        saved = back.Save(settingsPath);
    }
    if (!saved) {
        Log(L"[host] hotkey: %s, auto-return save failed - retry later", why);
        return false;
    }
    EnterCriticalSection(&g_hotkeyCs);
    g_hotkeyBorrowed = false;
    g_hotkeyBorrowTickMs = 0;
    LeaveCriticalSection(&g_hotkeyCs);
    ClearHotkeyState();
    Log(L"[host] hotkey: %s, auto-return to %s", why, ret.c_str());
    return true;
}

// Нажатие хоткея (main-поток, WM_HOTKEY): переключение — через settings.json,
// чтобы watcher хоста и UI остались согласованы.
// B6: весь Load→Save под g_settingsCs (vs worker StartRecording/auto-return).
void OnHotkeyPressed()
{
    std::wstring path = DefaultSettingsPath();
    if (path.empty()) {
        Log(L"[host] hotkey: settings path is empty");
        return;
    }
    SettingsFileGuard fsg;
    Settings s;
    if (!s.Load(path)) {
        Log(L"[host] hotkey: settings unreadable - ignored");
        return;
    }
    std::wstring display;
    EnterCriticalSection(&g_hotkeyCs);
    display = HotkeyDisplay(g_hotkey);
    bool borrowed = g_hotkeyBorrowed;
    std::wstring retType = g_hotkeyReturnType;
    LeaveCriticalSection(&g_hotkeyCs);

    if (!borrowed) {
        if (s.sourceType != L"video") {
            std::wstring from = s.sourceType;
            EnterCriticalSection(&g_hotkeyCs);
            g_hotkeyReturnType = from;
            g_hotkeyBorrowed = true;
            g_hotkeyBorrowTickMs = GetTickCount64(); // B5: старт backstop-таймаута
            LeaveCriticalSection(&g_hotkeyCs);
            s.sourceType = L"video";
            if (!s.Save(path)) {
                EnterCriticalSection(&g_hotkeyCs);
                g_hotkeyBorrowed = false;
                g_hotkeyBorrowTickMs = 0;
                LeaveCriticalSection(&g_hotkeyCs);
                Log(L"[host] hotkey %s: settings save failed - borrow cancelled",
                    display.c_str());
                return;
            }
            WriteHotkeyState(from);
            Log(L"[host] hotkey %s: %s -> video (play-once, auto-return to %s)",
                display.c_str(), from.c_str(), from.c_str());
        } else {
            // Ручное video в эфире (не borrow): хоткей уводит в static без borrow.
            s.sourceType = L"static";
            if (s.Save(path))
                Log(L"[host] hotkey %s: video -> static (manual video, no borrow)",
                    display.c_str());
            else
                Log(L"[host] hotkey %s: settings save failed", display.c_str());
        }
        return;
    }

    // Borrowed video в эфире: досрочный возврат на запомненное.
    // B5: та же семантика через helper (тиk сбрасывается внутри).
    std::wstring back = retType.empty() ? L"static" : retType;
    s.sourceType = back;
    if (!s.Save(path)) {
        Log(L"[host] hotkey %s: early return save failed - borrow kept", display.c_str());
        return;
    }
    EnterCriticalSection(&g_hotkeyCs);
    g_hotkeyBorrowed = false;
    g_hotkeyBorrowTickMs = 0;
    LeaveCriticalSection(&g_hotkeyCs);
    ClearHotkeyState();
    Log(L"[host] hotkey %s: early return video -> %s", display.c_str(), back.c_str());
}

// Нажатие хоткея записи (main-поток, WM_HOTKEY): только пишет команду —
// исполняет worker (рекордер живёт только там). Тоггл по state-файлу.
void OnRecordHotkeyPressed()
{
    std::wstring display;
    EnterCriticalSection(&g_hotkeyCs);
    display = HotkeyDisplay(g_recHotkey);
    LeaveCriticalSection(&g_hotkeyCs);

    std::wstring curPath;
    long long curStarted = 0;
    if (TryReadRecordState(curPath, curStarted)) {
        if (!WriteRecordCommand(L"stop", L""))
            Log(L"[host] record hotkey %s: stop command write failed", display.c_str());
        else
            Log(L"[host] record hotkey %s: stop requested", display.c_str());
        return;
    }
    // Запись не идёт — старт: путь из settings (пусто = дефолт сгенерирует worker).
    std::wstring want;
    std::wstring sp = DefaultSettingsPath();
    Settings s;
    if (!sp.empty() && s.Load(sp)) want = s.record.path;
    if (!WriteRecordCommand(L"start", want))
        Log(L"[host] record hotkey %s: start command write failed", display.c_str());
    else
        Log(L"[host] record hotkey %s: start requested", display.c_str());
}

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
    if (static_cast<HANDLE>(g_logFile) != INVALID_HANDLE_VALUE &&
        static_cast<HANDLE>(g_logFile) != nullptr) {
        char utf8[2200];
        int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (n > 2) {
            DWORD written = 0;
            WriteFile(static_cast<HANDLE>(g_logFile), utf8, (DWORD)(n - 1), &written,
                      nullptr); // без '\0'
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
        g_logFile.Attach(CreateFileW(path.c_str(), FILE_APPEND_DATA,
                                     FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (static_cast<HANDLE>(g_logFile) == INVALID_HANDLE_VALUE) {
            g_logFile.Detach();
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
    HANDLE raw = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle snap(raw);
    bool found = false;
    PROCESSENTRY32W pe = {};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Registrar.exe") == 0) { found = true; break; }
        } while (Process32NextW(snap, &pe));
    }
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
        ATL::CHandle h(OpenFileMappingW(FILE_MAP_READ, FALSE, name));
        if (!h) continue;
        bool active = false;
        vcam::MappedViewOfFilePtr view(MapViewOfFile(h, FILE_MAP_READ, 0, 0,
                                            sizeof(vcam::VCamSectionHeader)));
        if (view) {
            auto* hdr = view.GetAs<vcam::VCamSectionHeader>();
            if (hdr->magic == vcam::VCamMagic) {
                ULONGLONG tick = hdr->readerLastActiveTick;
                ULONGLONG now = GetTickCount64();
                active = (tick != 0) && (now >= tick) && (now - tick) <= kConsumerStaleMs;
            }
        }
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
    ATL::CHandle th(pi.hThread);
    ATL::CHandle proc(pi.hProcess); // detached: холдер переживает смерть хоста
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
    ATL::CHandle th(pi.hThread);
    ATL::CHandle proc(pi.hProcess);
    WaitForSingleObject(proc, 10000);
    DWORD rc = 1;
    GetExitCodeProcess(proc, &rc);
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
        ATL::CHandle th(pi.hThread);
        ATL::CHandle proc(pi.hProcess);
        WaitForSingleObject(proc, 15000);
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(proc, &code);
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
        ATL::CHandle proc(sei.hProcess);
        WaitForSingleObject(proc, 30000);
        DWORD code = (DWORD)-1;
        GetExitCodeProcess(proc, &code);
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
    // B6: перечитать под замком и писать свежее — иначе Load→schtasks→Save
    // (секунды UAC-диалога) затирает чужие правки worker/UI за это время.
    {
        SettingsFileGuard fsg;
        Settings fresh;
        if (!fresh.Load(path)) fresh = s; // файл пропал — пишем что помним
        fresh.autostart = want;
        if (!fresh.Save(path)) {
            Log(L"[host] toggle autostart: settings save failed (task already changed)");
            return;
        }
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

std::wstring ActiveStatus(const SourceConfig& cfg, const std::wstring& quality)
{
    return L"Источник: " + cfg.type + L" — " + TargetLabel(cfg) + L" (" + quality + L")";
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
    // Запись эфира в .mp4: кадры v1-кэша writer'а после ApplyFx (с эффектами).
    // Смена источника (BeginSwitch) рекордер не трогает — тот же файл дальше.
    Mp4Recorder rec;
    bool recErrLogged = false;   // one-shot лог ошибки записи (fail-open)
    uint64_t recLastDropped = 0; // последний залогированный счётчик дропов
    std::unique_ptr<IFrameSource> src;
    SourceConfig target;
    bool hasTarget = false;
    std::wstring quality = L"source"; // Settings.quality; смена -> переоткрытие
    // Эффекты (Settings.fx): применяются после RenderOne перед WriteOne;
    // смена только эффектов — без переоткрытия источника.
    EffectsSection fx;
    bool fxGpuLogged = false;  // one-shot лог fail-open эффектов
    bool fxFreiLogged = false; // one-shot лог CPU-fallback backend frei0r
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

// Старт записи (только worker): резолв пути команда → settings record.path →
// дефолт Videos\. Успешный старт с не-settings путём дописывает его в settings
// как дефолт на будущее (watcher-релоад без переоткрытия: record не в target).
void StartRecording(Machine& m, const std::wstring& requested)
{
    if (m.rec.IsOpen()) {
        Log(L"[host] record start ignored: already recording (%s)", m.rec.Path().c_str());
        return;
    }
    std::wstring settingsPath = DefaultSettingsPath();
    Settings cur;
    bool haveSettings = !settingsPath.empty() && cur.Load(settingsPath);
    std::wstring path = requested;
    if (path.empty() && haveSettings) path = cur.record.path;
    if (path.empty()) path = DefaultRecordPath();
    if (path.empty()) {
        Log(L"[host] record start: no path resolved (settings + default both empty)");
        return;
    }
    std::wstring err;
    if (!m.rec.Start(path, err)) {
        // fail-open: эфир продолжается, в логе причина
        Log(L"[host] record start failed: %s (%s)", path.c_str(), err.c_str());
        return;
    }
    m.recErrLogged = false;
    m.recLastDropped = 0;
    WriteRecordState(path);
    // B6: дефолт дописываем свежим под замком (cur загружен до Start — за это
    // время хоткей мог сменить sourceType; писать stale-копию нельзя).
    {
        SettingsFileGuard fsg;
        Settings fresh;
        if (!settingsPath.empty() && fresh.Load(settingsPath) && fresh.record.path != path) {
            fresh.record.path = path;
            if (fresh.Save(settingsPath))
                Log(L"[host] record: default path saved to settings");
            else
                Log(L"[host] record: settings save failed (recording continues)");
        }
    }
    Log(L"[host] record started: %s", path.c_str());
}

// Стоп записи (только worker): Finalize обязателен, иначе mp4 битый.
void StopRecording(Machine& m, const std::wstring& reason)
{
    if (!m.rec.IsOpen()) return;
    std::wstring path = m.rec.Path();
    uint64_t frames = m.rec.FramesWritten();
    uint64_t dropped = m.rec.FramesDropped();
    ULONGLONG ms = GetTickCount64() - m.rec.StartTickMs();
    m.rec.Stop(); // Finalize + освобождение
    ClearRecordState();
    std::wstring err = m.rec.LastError();
    Log(L"[host] record stopped (%s): %s (frames=%llu, %.1fs, dropped=%llu%s%s)",
        reason.c_str(), path.c_str(), (unsigned long long)frames, ms / 1000.0,
        (unsigned long long)dropped,
        err.empty() ? L"" : L", ", err.empty() ? L"" : err.c_str());
}

// Один эфирный кадр в запись: вызывается после УСПЕШНОГО WriteOne, т.е. кэш
// writer'а свежий и уже с эффектами. Ошибка записи — стоп записи, эфир живёт.
void RecordEtherFrame(Machine& m)
{
    if (!m.rec.IsOpen()) return;
    if (!m.writer.HasFrame720p()) return;
    const auto& f = m.writer.LastFrame720p();
    if (f.size() < Mp4Recorder::kFrameSize) return;
    if (!m.rec.WriteFrame720p(f.data())) {
        if (!m.recErrLogged) {
            m.recErrLogged = true;
            Log(L"[host] record write failed: %s - recording stopped, эфир продолжается (fail-open)",
                m.rec.LastError().c_str());
        }
        StopRecording(m, L"ошибка записи");
        return;
    }
    if (m.rec.FramesDropped() != m.recLastDropped) {
        m.recLastDropped = m.rec.FramesDropped();
        Log(L"[host] record: encoder lags, dropped=%llu (эфир не ждёт)",
            (unsigned long long)m.recLastDropped);
    }
}

// Эффекты хоста: после успешного RenderOne, перед WriteOne, над m.buf
// (буфер плотно упакован frameW x frameH BGRX — stride frameW*4).
// Единая точка для всех фаз (Switch/Active/Fallback-restore).
// Исполнитель — GpuEffects (GPU mirror/grayscale + CPU-аналог помех);
// false = fail-open: кадр без изменений.
void ApplyFx(Machine& m)
{
    if (!m.fx.enabled) return; // мастер-выключатель: эффекты скипаются целиком
    vcam::effects::FxFlags f;
    f.mirror = m.fx.mirror;
    f.grayscale = m.fx.grayscale;
    f.noise = m.fx.noise;
    f.scanlines = m.fx.scanlines;
    f.rgbSplit = m.fx.rgbSplit;
    f.tracking = m.fx.tracking;
    f.vhs = m.fx.vhs;
    f.noiseLevel = m.fx.noiseLevel;
    f.scanlinesLevel = m.fx.scanlinesLevel;
    f.rgbSplitLevel = m.fx.rgbSplitLevel;
    f.trackingLevel = m.fx.trackingLevel;
    f.backend = m.fx.backend;
    const bool ok = vcam::effects::ApplyEffects(m.buf.data(), (int)(m.frameW * 4),
                                                m.frameW, m.frameH, f);
    if (!ok && !m.fxGpuLogged) {
        m.fxGpuLogged = true;
        Log(L"[host] effects: сбой эффектов, кадры идут без них (fail-open)");
    }
    // frei0r недоступен (нет DLL/init fail) — помехи посчитаны CPU, кадр
    // в эфире; логируем один раз (флаг сбрасывается при смене backend).
    if (ok && vcam::effects::TakeFreiFallbackFlag() && !m.fxFreiLogged) {
        m.fxFreiLogged = true;
        Log(L"[host] effects: backend frei0r недоступен, помехи на CPU (fail-open)");
    }
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
    SetStatus(FallbackStatus(reason));
    Log(L"[host] no signal: %s", reason.c_str());
}

void SetActiveStatus(Machine& m)
{
    if (m.writerOpen) SetStatus(ActiveStatus(m.target, m.quality));
    else SetStatus(FallbackStatus(m.writerErr));
}

void BeginSwitch(Machine& m, const SourceConfig& want, const std::wstring& quality)
{
    std::wstring wantLabel = TargetLabel(want);
    Log(L"[host] switch: type=%s path=%s quality=%s (was type=%s path=%s quality=%s)",
        want.type.c_str(), wantLabel.c_str(), quality.c_str(),
        m.hasTarget ? m.target.type.c_str() : L"-",
        m.hasTarget ? TargetLabel(m.target).c_str() : L"-",
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
        if (RenderOne(m.src.get(), m, rerr)) {
            ApplyFx(m);
            bool written = WriteOne(m);
            if (written) RecordEtherFrame(m);
            m.phase = Phase::Active;
            SetActiveStatus(m);
            Log(L"[host] active: type=%s path=%s%s [%s %ux%u quality=%s]",
                m.target.type.c_str(), TargetLabel(m.target).c_str(),
                written ? L"" : L" (write failed)",
                m.nativeKnown ? L"native" : L"720p",
                m.frameW, m.frameH, m.quality.c_str());
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
        if (m.src && RenderOne(m.src.get(), m, rerr)) {
            ApplyFx(m);
            if (WriteOne(m)) {
                RecordEtherFrame(m);
                return 0;
            }
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
                    if (RenderOne(cand.get(), m, rerr)) {
                        m.src = std::move(cand);
                        ApplyFx(m);
                        if (WriteOne(m)) RecordEtherFrame(m);
                        m.phase = Phase::Active;
                        SetActiveStatus(m);
                        Log(L"[host] signal restored: type=%s path=%s [%s %ux%u quality=%s]",
                            m.target.type.c_str(), TargetLabel(m.target).c_str(),
                            m.nativeKnown ? L"native" : L"720p",
                            m.frameW, m.frameH, m.quality.c_str());
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
        if (m.writer.IsV2Open())
            Log(L"[host] v2 writer ready (%s)", m.writer.V2SectionOpenedAs().c_str());
        else
            Log(L"[host] v2 writer unavailable (v1-only mode)");
    } else {
        m.writerErr = err.empty() ? L"writer open failed" : err;
        Log(L"[host] writer open failed: %s", err.c_str());
        SetStatus(FallbackStatus(m.writerErr));
    }

    // Зависшие transient-файлы записи от краша: single-instance гарантирует,
    // что чужой записи нет — запись НЕ возобновляем, команду НЕ исполняем.
    // К2: чистим и недоеденный *.processing (краш между move и delete).
    {
        std::wstring cp = RecordCommandPath();
        if (!cp.empty() && GetFileAttributesW(cp.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(cp.c_str());
            Log(L"[host] record: stale command removed (not resuming after restart)");
        }
        if (!cp.empty()) {
            std::wstring proc = cp + L".processing";
            if (GetFileAttributesW(proc.c_str()) != INVALID_FILE_ATTRIBUTES) {
                DeleteFileW(proc.c_str());
                Log(L"[host] record: stale command processing removed (not resuming after restart)");
            }
        }
        std::wstring curPath;
        long long curStarted = 0;
        if (TryReadRecordState(curPath, curStarted)) {
            ClearRecordState();
            Log(L"[host] record: stale state removed (was: %s)", curPath.c_str());
        }
    }

    HANDLE waits[2] = { static_cast<HANDLE>(g_stop), static_cast<HANDLE>(g_dirty) };
    bool first = true;
    DWORD timeout = 0;
    const std::wstring settingsPath = DefaultSettingsPath();
    HotkeySection curHotkey; // последний виденный в settings (дефолт = Ctrl+Alt+V)
    RecordHotkeySection curRecHotkey; // последний виденный (дефолт = Ctrl+Alt+R)
    for (;;) {
        DWORD r = WaitForMultipleObjects(2, waits, FALSE, timeout);
        if (r == WAIT_OBJECT_0) break;
        if (first || r == WAIT_OBJECT_0 + 1) {
            first = false;
            Settings s;
            g_watcher.Current(s);
            // Смена только хоткея — перерегистрация, без переоткрытия источника
            // (Settings::operator== включает hotkey, поэтому watcher шлёт dirty).
            if (s.hotkey != curHotkey) {
                curHotkey = s.hotkey;
                EnterCriticalSection(&g_hotkeyCs);
                g_hotkey = s.hotkey;
                LeaveCriticalSection(&g_hotkeyCs);
                PostMessageW(g_hwnd, WM_REAPPLY_HOTKEY, 0, 0);
            }
            // Смена только хоткея записи — перерегистрация, без переоткрытия.
            if (s.recordHotkey != curRecHotkey) {
                curRecHotkey = s.recordHotkey;
                EnterCriticalSection(&g_hotkeyCs);
                g_recHotkey = s.recordHotkey;
                LeaveCriticalSection(&g_hotkeyCs);
                PostMessageW(g_hwnd, WM_REAPPLY_HOTKEY, 0, 0);
            }
            SourceConfig want = ToSourceConfig(s);
            EnterCriticalSection(&g_hotkeyCs);
            bool borrowed = g_hotkeyBorrowed;
            LeaveCriticalSection(&g_hotkeyCs);
            // Пользователь ушёл из borrowed-video вручную (не хоткеем):
            // borrow снят, автовозврата не будет.
            if (borrowed && want.type != L"video") {
                EnterCriticalSection(&g_hotkeyCs);
                g_hotkeyBorrowed = false;
                g_hotkeyBorrowTickMs = 0; // B5
                LeaveCriticalSection(&g_hotkeyCs);
                ClearHotkeyState();
                Log(L"[host] hotkey: manual switch away from borrowed video - borrow dropped");
                borrowed = false;
            }
            // Заимствованное video играется один раз (конец файла = Ended() =
            // авто-возврат); обычное video — луп как раньше.
            if (borrowed && want.type == L"video") want.playOnce = true;
            if (!m.hasTarget || want != m.target || s.quality != m.quality)
                BeginSwitch(m, want, s.quality);
            // Смена только эффектов — без переоткрытия источника: флаги
            // подхватываются на лету (вотчер шлёт dirty через operator== с fx).
            if (s.fx != m.fx) {
                if (s.fx.backend != m.fx.backend) m.fxFreiLogged = false;
                m.fx = s.fx;
                Log(L"[host] effects: enabled=%d mirror=%d grayscale=%d noise=%d(%d) scanlines=%d(%d) rgbsplit=%d(%d) tracking=%d(%d) vhs=%d backend=%s",
                    (int)m.fx.enabled, (int)m.fx.mirror, (int)m.fx.grayscale, (int)m.fx.noise,
                    m.fx.noiseLevel, (int)m.fx.scanlines, m.fx.scanlinesLevel,
                    (int)m.fx.rgbSplit, m.fx.rgbSplitLevel, (int)m.fx.tracking,
                    m.fx.trackingLevel, (int)m.fx.vhs, m.fx.backend.c_str());
            }
            // record {path} / recordHotkey в operator== дают watcher-dirty, но
            // переоткрытия не требуют (не входят в SourceConfig/quality/fx).
        }
        // Команды записи UI/хоткея — каждую итерацию (дешёвый опрос файла).
        {
            std::wstring rcmd, rpath;
            if (ConsumeRecordCommand(rcmd, rpath)) {
                if (rcmd == L"start") StartRecording(m, rpath);
                else if (rcmd == L"stop") StopRecording(m, L"команда UI/хоткея");
                else Log(L"[host] record: unknown command ignored");
            }
        }
        timeout = Step(m);
        // Конец заимствованного ролика: авто-возврат на запомненный источник
        // через settings.json (watcher подхватит как обычное переключение).
        // B5: раньше — только Ended(). Битый ролик уходит в Fallback (open/
        // render fail), Ended не fires — borrow висел вечно и UI врал
        // «идёт видео». Теперь возврат и из Fallback + backstop-таймаут.
        EnterCriticalSection(&g_hotkeyCs);
        bool borrowedNow = g_hotkeyBorrowed;
        ULONGLONG borrowTick = g_hotkeyBorrowTickMs;
        LeaveCriticalSection(&g_hotkeyCs);
        if (borrowedNow) {
            bool ended = m.src && m.src->Ended();
            ULONGLONG nowBorrow = GetTickCount64();
            bool timedOut = borrowTick != 0 && nowBorrow - borrowTick > kBorrowMaxMs;
            if (ended)
                AutoReturnBorrowedVideo(settingsPath, L"video ended");
            else if (m.phase == Phase::Fallback)
                AutoReturnBorrowedVideo(settingsPath, L"fallback during borrowed video");
            else if (timedOut)
                AutoReturnBorrowedVideo(settingsPath, L"borrow timeout");
        }
    }

    // Запись обязана финализироваться (Finalize в Stop), иначе mp4 битый.
    // До writer.Close: порядок не важен, но запись — до выхода точно.
    if (m.rec.IsOpen()) StopRecording(m, L"выход хоста");
    CloseSource(m);
    m.writer.Close();
    vcam::effects::ShutdownEffects();
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
    EnterCriticalSection(&g_hotkeyCs);
    bool borrowedMenu = g_hotkeyBorrowed;
    std::wstring retMenu = g_hotkeyReturnType;
    LeaveCriticalSection(&g_hotkeyCs);
    if (borrowedMenu)
        status += L" [видео по хоткею, автовозврат → " +
                  (retMenu.empty() ? std::wstring(L"static") : retMenu) + L"]";
    // Индикатор записи из transient state-файла (main-поток, рекордер не трогаем).
    {
        std::wstring recPath;
        long long recStarted = 0;
        if (TryReadRecordState(recPath, recStarted)) {
            long long el = (long long)time(nullptr) - recStarted;
            if (el < 0) el = 0;
            wchar_t t[32];
            swprintf_s(t, L"%02lld:%02lld", el / 60, el % 60);
            size_t bs = recPath.find_last_of(L"\\/");
            std::wstring fn = (bs == std::wstring::npos) ? recPath : recPath.substr(bs + 1);
            status += L" [● REC ";
            status += t;
            status += L" ";
            status += fn;
            status += L"]";
        }
    }
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
    case WM_HOTKEY:
        if (wp == kHotkeyId) OnHotkeyPressed();
        else if (wp == kRecHotkeyId) OnRecordHotkeyPressed();
        return 0;
    case WM_REAPPLY_HOTKEY:
        ApplyHotkeyRegistration();
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
    HANDLE rawToken = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &rawToken)) {
        Log(L"[host] token: OpenProcessToken failed: %lu", GetLastError());
        return;
    }
    ATL::CHandle token(rawToken);
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
    Log(L"[host] token: elevated=%d SeCreateGlobalPrivilege=%d", elevated, priv);
}

int APIENTRY wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
    g_inst = hInstance;
    InitLogging();
    Log(L"[host] VCamVideoStreamProducer starting");
    LogTokenState();

    SetLastError(ERROR_SUCCESS);
    g_mutex.Attach(CreateMutexW(nullptr, FALSE, kMutexName));
    if (static_cast<HANDLE>(g_mutex) == nullptr) {
        Log(L"[host] CreateMutex failed: %lu", GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        Log(L"[host] already running - second instance exits");
        MessageBoxW(nullptr,
                    L"Хост VCam уже запущен (VCamVideoStreamProducer.exe).\n"
                    L"Используйте значок в трее рядом с часами.",
                    L"VCam Video Stream Producer", MB_OK | MB_ICONINFORMATION);
        g_mutex.Close();
        return 0;
    }
    WaitForSingleObject(static_cast<HANDLE>(g_mutex), INFINITE); // владение мьютексом = «хост запущен»

    g_stop.Attach(CreateEventW(nullptr, TRUE, FALSE, kStopEventName));
    if (static_cast<HANDLE>(g_stop) == nullptr) {
        Log(L"[host] CreateEvent(Stop) failed: %lu", GetLastError());
        ReleaseMutex(g_mutex);
        g_mutex.Close();
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) ResetEvent(g_stop);
    g_dirty.Attach(CreateEventW(nullptr, FALSE, FALSE, nullptr));
    InitializeCriticalSection(&g_statusCs);
    InitializeCriticalSection(&g_hotkeyCs);
    InitializeCriticalSection(&g_settingsCs); // B6

    ApplyAutostartFromSettings();

    WNDCLASSEXW wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_inst;
    // Иконка exe (version.rc: 1 ICON). Нет ресурса — стандартный IDI_APPLICATION.
    wc.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    if (!wc.hIcon) wc.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    wc.hIconSm = wc.hIcon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kTrayClass;
    if (!RegisterClassExW(&wc)) {
        Log(L"[host] RegisterClassEx failed: %lu", GetLastError());
    }
    g_hwnd = CreateWindowExW(0, kTrayClass, L"VCamVideoStreamProducer", WS_OVERLAPPED,
                             0, 0, 0, 0, nullptr, nullptr, g_inst, nullptr);
    if (!g_hwnd) {
        Log(L"[host] CreateWindowEx failed: %lu", GetLastError());
        DeleteCriticalSection(&g_settingsCs); // B6
        DeleteCriticalSection(&g_hotkeyCs);
        DeleteCriticalSection(&g_statusCs);
        g_dirty.Close();
        g_stop.Close();
        ReleaseMutex(g_mutex);
        g_mutex.Close();
        return 1;
    }

    // Стартовые хоткеи из settings.json (мусор уже нормализован в дефолт
    // парсером Settings).
    {
        Settings initS;
        if (initS.Load(DefaultSettingsPath())) {
            EnterCriticalSection(&g_hotkeyCs);
            g_hotkey = initS.hotkey;
            g_recHotkey = initS.recordHotkey;
            LeaveCriticalSection(&g_hotkeyCs);
        }
    }
    ApplyHotkeyRegistration();

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    g_nid.hIcon = LoadIconW(g_inst, MAKEINTRESOURCEW(1));
    if (!g_nid.hIcon) g_nid.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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

    g_worker.Attach(CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr));
    if (static_cast<HANDLE>(g_worker) == nullptr)
        Log(L"[host] CreateThread(worker) failed: %lu", GetLastError());

    for (;;) {
        MSG msg;
        bool quit = false;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { quit = true; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (quit) break;
        HANDLE hStop = static_cast<HANDLE>(g_stop);
        DWORD r = MsgWaitForMultipleObjects(1, &hStop, FALSE, INFINITE, QS_ALLINPUT);
        if (r == WAIT_OBJECT_0) break;
    }

    Log(L"[host] shutting down");
    SetEvent(g_stop);
    g_watcher.Stop();
    if (static_cast<HANDLE>(g_worker) != nullptr) {
        // Handles/critical sections below are shared with the worker: it must
        // be joined before they are destroyed (checked wait, not best-effort).
        if (WaitForSingleObject(static_cast<HANDLE>(g_worker), 8000) != WAIT_OBJECT_0) {
            Log(L"[host] worker did not stop in 8000 ms; waiting indefinitely");
            WaitForSingleObject(static_cast<HANDLE>(g_worker), INFINITE);
        }
        g_worker.Close();
    }
    Shell_NotifyIconW(NIM_DELETE, &g_nid);
    Log(L"[host] tray icon removed");
    UnregisterHotKey(g_hwnd, kHotkeyId);
    UnregisterHotKey(g_hwnd, kRecHotkeyId);
    DestroyWindow(g_hwnd);
    g_hwnd = nullptr;

    g_dirty.Close();
    g_stop.Close();
    DeleteCriticalSection(&g_settingsCs); // B6
    DeleteCriticalSection(&g_hotkeyCs);
    DeleteCriticalSection(&g_statusCs);
    ReleaseMutex(g_mutex);
    g_mutex.Close();
    Log(L"[host] exit");
    g_logFile.Close();
    return 0;
}
