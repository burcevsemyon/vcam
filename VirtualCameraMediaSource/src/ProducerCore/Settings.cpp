#include "Settings.h"
#include "FailOpenCounters.h"

#include <windows.h>
#include <atlbase.h>

#include <cstdio>
#include <cwctype>
#include <string>

namespace {

std::wstring Utf8ToWide(const std::string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    if (n <= 0) return std::wstring();
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}

bool ReadUtf8File(const std::wstring& path, std::string& out)
{
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1000000) return false;
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    return ok && read == out.size();
}

// B6: атомарная запись через tmp+move (как host WriteRecordCommand):
// конкурентный читатель (UI/host-рид) видит старый или новый файл целиком,
// рваного truncate-read нет. Плюс retry при sharing-violation — вторая
// сторона (C# Save тоже tmp+move) может держать файл в момент move.
bool WriteUtf8FileNoBom(const std::wstring& path, const std::string& content)
{
    // Уникальный tmp на запись (хост vs UI пишут одновременно; плюс
    // g_settingsCs сериализует только потоки хоста): общий фиксированный tmp
    // сталкивал писателей sharing-violation.
    wchar_t uniq[48];
    swprintf(uniq, 48, L".%lu.%llu.tmp", GetCurrentProcessId(),
             (unsigned long long)GetTickCount64());
    std::wstring tmp = path + uniq;
    for (int attempt = 0;; ++attempt) {
        HANDLE raw = CreateFileW(tmp.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
            CREATE_ALWAYS, 0, nullptr);
        if (raw == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            if (attempt >= 2 || (e != ERROR_SHARING_VIOLATION &&
                                 e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION))
                return false;
            Sleep(50);
            continue;
        }
        ATL::CHandle h(raw);
        DWORD written = 0;
        BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
        h.Close();
        if (!ok || written != content.size()) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        break;
    }
    for (int attempt = 0;; ++attempt) {
        if (MoveFileWithProgressW(tmp.c_str(), path.c_str(), nullptr, nullptr,
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return true;
        DWORD e = GetLastError();
        if (attempt >= 2 || (e != ERROR_SHARING_VIOLATION &&
                             e != ERROR_ACCESS_DENIED && e != ERROR_LOCK_VIOLATION)) {
            DeleteFileW(tmp.c_str());
            return false;
        }
        Sleep(50);
    }
}

void SkipWs(const std::string& json, size_t& p)
{
    while (p < json.size() && (json[p] == ' ' || json[p] == '\t' || json[p] == '\r' || json[p] == '\n')) p++;
}

// Плоский парсер в стиле JsonGetString/GetInt/GetBool из StaticProducer/VideoProducer:
// ищет "key" и значение после ':' внутри переданного фрагмента JSON.
bool JsonGetString(const std::string& json, const char* key, std::wstring& value)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return false;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return false;
    p++;
    SkipWs(json, p);
    if (p >= json.size() || json[p] != '"') return false;
    std::string val;
    for (p++; p < json.size() && json[p] != '"'; p++) {
        if (json[p] == '\\' && p + 1 < json.size()) {
            char c = json[++p];
            switch (c) {
                case 'n': val += '\n'; break;
                case 'r': val += '\r'; break;
                case 't': val += '\t'; break;
                case 'b': val += '\b'; break;
                case 'f': val += '\f'; break;
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
    value = Utf8ToWide(val);
    return true;
}

int JsonGetInt(const std::string& json, const char* key, int defaultVal)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return defaultVal;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return defaultVal;
    p++;
    SkipWs(json, p);
    bool neg = false;
    if (p < json.size() && json[p] == '-') { neg = true; p++; }
    if (p >= json.size() || json[p] < '0' || json[p] > '9') return defaultVal;
    long v = 0;
    for (; p < json.size() && json[p] >= '0' && json[p] <= '9'; p++) {
        v = v * 10 + (json[p] - '0');
        if (v > 1000000) break;
    }
    return neg ? (int)-v : (int)v;
}

bool JsonGetBool(const std::string& json, const char* key, bool defaultVal)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k);
    if (p == std::string::npos) return defaultVal;
    p = json.find(':', p + k.size());
    if (p == std::string::npos) return defaultVal;
    p++;
    SkipWs(json, p);
    if (p + 4 <= json.size() && json.compare(p, 4, "true") == 0) return true;
    if (p + 5 <= json.size() && json.compare(p, 5, "false") == 0) return false;
    return defaultVal;
}

// Позиция ключа "key", за которым действительно следует ':' (а не значение,
// совпадающее с именем ключа — например "type": "video" vs ключ "video").
size_t FindKeyPos(const std::string& json, const char* key, size_t from = 0)
{
    std::string k = std::string("\"") + key + "\"";
    size_t p = json.find(k, from);
    while (p != std::string::npos) {
        size_t c = p + k.size();
        SkipWs(json, c);
        if (c < json.size() && json[c] == ':') return p;
        p = json.find(k, p + 1);
    }
    return std::string::npos;
}

// true, если в JSON есть хотя бы один известный ключ (новой или legacy-схемы).
// Пусто/битый файл без распознанных ключей = fail-open «дефолт» (P0.2).
bool HasAnyKnownKey(const std::string& json)
{
    static const char* keys[] = {
        // новая схема
        "source", "static", "video", "camera", "quality", "hotkey",
        "recordHotkey", "videoHotkey", "record", "autostart",
        "sourceStaticHotkey", "sourceVideoHotkey", "sourceCameraHotkey",
        // legacy-схема
        "imagePath", "mediaPath", "mediaMode", "scaleMode",
        "cropX", "cropY", "cropW", "cropH", "cropKeepAspect"
    };
    for (const char* k : keys) {
        if (FindKeyPos(json, k) != std::string::npos) return true;
    }
    return false;
}

// Возвращает границы содержимого объекта-значения ключа: [begin, end) между '{' и '}'.
bool FindObjectRange(const std::string& json, const char* key, size_t& begin, size_t& end)
{
    size_t p = FindKeyPos(json, key);
    if (p == std::string::npos) return false;
    p = json.find(':', p);
    if (p == std::string::npos) return false;
    p++;
    SkipWs(json, p);
    if (p >= json.size() || json[p] != '{') return false;
    size_t open = p;
    int depth = 0;
    bool inStr = false;
    for (; p < json.size(); p++) {
        char c = json[p];
        if (inStr) {
            if (c == '\\') { p++; continue; }
            if (c == '"') inStr = false;
            continue;
        }
        if (c == '"') { inStr = true; continue; }
        if (c == '{') depth++;
        else if (c == '}') {
            depth--;
            if (depth == 0) { begin = open + 1; end = p; return true; }
        }
    }
    return false;
}

std::string EscapeJson(const std::wstring& w)
{
    std::string s = WideToUtf8(w);
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if ((unsigned char)c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", (unsigned char)c);
                    out += buf;
                } else {
                    out += c;
                }
        }
    }
    return out;
}

// К4: ASCII case-insensitive compare (для JSON-токенов; C# —
// OrdinalIgnoreCase). Юникод-фолдинг не нужен: токены строго ASCII.
bool EqCI(const std::wstring& a, const wchar_t* b)
{
    size_t n = wcslen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        wchar_t ca = a[i], cb = b[i];
        if (ca >= L'A' && ca <= L'Z') ca = (wchar_t)(ca + (L'a' - L'A'));
        if (cb >= L'A' && cb <= L'Z') cb = (wchar_t)(cb + (L'a' - L'A'));
        if (ca != cb) return false;
    }
    return true;
}

bool ParseScaleMode(const std::wstring& m, std::wstring& out)
{
    // К4: C# читает scaleMode через OrdinalIgnoreCase, C++ сравнивал exact —
    // "FIT"/"Cover" давали fit в UI и cover в хосте. Унифицировано: обе
    // стороны insensitive, канон — нижний регистр. Возврат как раньше:
    // true = токен распознан, false = fallback fit.
    if (EqCI(m, L"fit")) { out = L"fit"; return true; }
    if (EqCI(m, L"cover")) { out = L"cover"; return true; }
    if (EqCI(m, L"crop")) { out = L"crop"; return true; }
    out = L"fit";
    return false;
}

// quality: только source | fixed1080p | fixed720p, остальное -> source.
// Лесенка только вниз (как C# ParseQuality — ordinal, синхронно).
void ParseQuality(const std::wstring& q, std::wstring& out)
{
    out = (q == L"fixed720p") ? L"fixed720p" : (q == L"fixed1080p") ? L"fixed1080p" : L"source";
}

// camera.capture: только 720p | 1080p, остальное (включая отсутствие) -> max.
void ParseCapture(const std::wstring& c, std::wstring& out)
{
    out = (c == L"720p") ? L"720p" : (c == L"1080p") ? L"1080p" : L"max";
}

void ParseNewSchema(const std::string& json, Settings& s)
{
    size_t b = 0, e = 0;
    if (FindObjectRange(json, "source", b, e)) {
        std::wstring t;
        if (JsonGetString(json.substr(b, e - b), "type", t) && !t.empty()) {
            s.sourceType = t; // неизвестный тип сохраняем: хост уйдёт в fallback (NO SIGNAL)
        }
    }
    if (FindObjectRange(json, "static", b, e)) {
        std::string sec = json.substr(b, e - b);
        JsonGetString(sec, "path", s.st.path);
        std::wstring sm;
        if (JsonGetString(sec, "scaleMode", sm)) ParseScaleMode(sm, s.st.scaleMode);
        s.st.cropX = JsonGetInt(sec, "cropX", 0);
        s.st.cropY = JsonGetInt(sec, "cropY", 0);
        s.st.cropW = JsonGetInt(sec, "cropW", 0);
        s.st.cropH = JsonGetInt(sec, "cropH", 0);
        s.st.cropKeepAspect = JsonGetBool(sec, "cropKeepAspect", false);
    }
    if (FindObjectRange(json, "video", b, e)) {
        std::string sec = json.substr(b, e - b);
        JsonGetString(sec, "path", s.video.path);
        s.video.loop = JsonGetBool(sec, "loop", false);
    }
    if (FindObjectRange(json, "camera", b, e)) {
        std::string sec = json.substr(b, e - b);
        JsonGetString(sec, "id", s.cam.id);
        JsonGetString(sec, "name", s.cam.name);
        std::wstring cap;
        if (JsonGetString(sec, "capture", cap)) ParseCapture(cap, s.cam.capture);
    }
    // Секция "effects" (legacy эффектов) больше не читается: старые файлы
    // с ней принимаются без ошибок — неизвестная секция просто игнорируется.
    std::wstring q;
    if (JsonGetString(json, "quality", q)) ParseQuality(q, s.quality);
    s.autostart = JsonGetBool(json, "autostart", true);
    if (FindObjectRange(json, "hotkey", b, e)) {
        std::string sec = json.substr(b, e - b);
        // hotkey: {modifiers, vk}; мусор (включая 0) → дефолт Ctrl+Alt+V
        // (модификаторы — биты MOD_ALT|CONTROL|SHIFT|WIN = 1|2|4|8).
        int mods = JsonGetInt(sec, "modifiers", 3);
        s.hotkey.modifiers = (mods >= 1 && mods <= 15) ? mods : 3;
        int vk = JsonGetInt(sec, "vk", 0x56);
        s.hotkey.vk = (vk >= 0x08 && vk <= 0xFE) ? vk : 0x56;
    }
    if (FindObjectRange(json, "recordHotkey", b, e)) {
        std::string sec = json.substr(b, e - b);
        // recordHotkey: те же правила, дефолт Ctrl+Alt+R.
        int mods = JsonGetInt(sec, "modifiers", 3);
        s.recordHotkey.modifiers = (mods >= 1 && mods <= 15) ? mods : 3;
        int vk = JsonGetInt(sec, "vk", 0x52);
        s.recordHotkey.vk = (vk >= 0x08 && vk <= 0xFE) ? vk : 0x52;
    }
    if (FindObjectRange(json, "videoHotkey", b, e)) {
        std::string sec = json.substr(b, e - b);
        // videoHotkey: те же правила, дефолт Ctrl+Alt+P.
        int mods = JsonGetInt(sec, "modifiers", 3);
        s.videoHotkey.modifiers = (mods >= 1 && mods <= 15) ? mods : 3;
        int vk = JsonGetInt(sec, "vk", 0x50);
        s.videoHotkey.vk = (vk >= 0x08 && vk <= 0xFE) ? vk : 0x50;
    }
    if (FindObjectRange(json, "record", b, e)) {
        JsonGetString(json.substr(b, e - b), "path", s.record.path);
    }
    // sourceStaticHotkey/sourceVideoHotkey/sourceCameraHotkey: те же правила,
    // что у hotkey; дефолты Ctrl+Alt+1/2/3 (мусор/ноль → дефолт секции).
    struct { const char* key; int defVk; SourceSwitchHotkeySection* out; } shks[] = {
        { "sourceStaticHotkey", 0x31, &s.sourceStaticHotkey },
        { "sourceVideoHotkey",  0x32, &s.sourceVideoHotkey },
        { "sourceCameraHotkey", 0x33, &s.sourceCameraHotkey },
    };
    for (auto& shk : shks) {
        if (!FindObjectRange(json, shk.key, b, e)) continue;
        std::string sec = json.substr(b, e - b);
        int mods = JsonGetInt(sec, "modifiers", 3);
        shk.out->modifiers = (mods >= 1 && mods <= 15) ? mods : 3;
        int vk = JsonGetInt(sec, "vk", shk.defVk);
        shk.out->vk = (vk >= 0x08 && vk <= 0xFE) ? vk : shk.defVk;
    }
}

// Старый формат: корневые imagePath/mediaMode/mediaPath/scaleMode/crop*.
void ParseLegacySchema(const std::string& json, Settings& s)
{
    std::wstring imagePath, mediaPath, mediaMode;
    JsonGetString(json, "imagePath", imagePath);
    JsonGetString(json, "mediaPath", mediaPath);
    JsonGetString(json, "mediaMode", mediaMode);

    // К4: mediaMode — insensitive как в C# (там OrdinalIgnoreCase "video"),
    // иначе legacy {"mediaMode":"Video"} давал static в хосте и video в UI.
    const bool isVideo = EqCI(mediaMode, L"video");
    s.sourceType = isVideo ? L"video" : L"static";
    s.st.path = imagePath;
    if (s.st.path.empty() && !isVideo) s.st.path = mediaPath;

    std::wstring sm;
    if (JsonGetString(json, "scaleMode", sm)) ParseScaleMode(sm, s.st.scaleMode);
    s.st.cropX = JsonGetInt(json, "cropX", 0);
    s.st.cropY = JsonGetInt(json, "cropY", 0);
    s.st.cropW = JsonGetInt(json, "cropW", 0);
    s.st.cropH = JsonGetInt(json, "cropH", 0);
    s.st.cropKeepAspect = JsonGetBool(json, "cropKeepAspect", false);

    s.video.path = mediaPath;
    s.video.loop = false; // legacy без ключа loop -> один проход (default)
    s.quality = L"source"; // legacy без ключа quality -> source
    s.cam.capture = L"max"; // legacy без секции camera -> max
    s.hotkey.modifiers = 3; // legacy без секции hotkey -> Ctrl+Alt+V
    s.hotkey.vk = 0x56;
    s.recordHotkey.modifiers = 3; // legacy без recordHotkey -> Ctrl+Alt+R
    s.recordHotkey.vk = 0x52;
    s.videoHotkey.modifiers = 3; // legacy без videoHotkey -> Ctrl+Alt+P
    s.videoHotkey.vk = 0x50;
    s.sourceStaticHotkey.modifiers = 3; // legacy без source*Hotkey -> Ctrl+Alt+1/2/3
    s.sourceStaticHotkey.vk = 0x31;
    s.sourceVideoHotkey.modifiers = 3;
    s.sourceVideoHotkey.vk = 0x32;
    s.sourceCameraHotkey.modifiers = 3;
    s.sourceCameraHotkey.vk = 0x33;
    s.record.path.clear(); // legacy без секции record -> дефолт хоста (Videos\...)
    s.autostart = JsonGetBool(json, "autostart", true);
}

} // namespace

std::wstring DefaultSettingsPath()
{
    wchar_t appdata[MAX_PATH] = {};
    DWORD n = GetEnvironmentVariableW(L"APPDATA", appdata, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return std::wstring();
    return std::wstring(appdata) + L"\\VCam\\settings.json";
}

bool Settings::Load(const std::wstring& path)
{
    std::string json;
    if (!ReadUtf8File(path, json)) return false;

    size_t b = 0, e = 0;
    bool isNew = FindObjectRange(json, "source", b, e) ||
                 FindObjectRange(json, "static", b, e) ||
                 FindObjectRange(json, "video", b, e) ||
                 FindObjectRange(json, "camera", b, e);
    if (isNew) ParseNewSchema(json, *this);
    else ParseLegacySchema(json, *this);

    if (sourceType.empty()) sourceType = L"static";
    std::wstring sm;
    if (ParseScaleMode(st.scaleMode, sm)) st.scaleMode = sm;

    // fail-open (P0.2): файл прочитан, но ни одного известного ключа не найдено →
    // битый/чужой JSON, молча применены дефолты. Счётчик + флаг; строку в лог
    // пишет SettingsWatcher (у Settings нет доступа к Log). Пустой файл = норма.
    if (json.find_first_not_of(" \t\r\n") == std::string::npos) {
        brokenJson_ = false;
    } else if (!HasAnyKnownKey(json)) {
        vcam::IncFailOpen(vcam::FailOpen::SettingsBrokenJson);
        brokenJson_ = true;
    } else {
        brokenJson_ = false;
    }
    return true;
}

std::string Settings::Serialize() const
{
    std::string out;
    out += "{\n";
    out += "  \"source\": { \"type\": \"" + EscapeJson(sourceType) + "\" },\n";
    out += "  \"static\": { \"path\": \"" + EscapeJson(st.path) +
           "\", \"scaleMode\": \"" + EscapeJson(st.scaleMode) +
           "\", \"cropX\": " + std::to_string(st.cropX) +
           ", \"cropY\": " + std::to_string(st.cropY) +
           ", \"cropW\": " + std::to_string(st.cropW) +
           ", \"cropH\": " + std::to_string(st.cropH) +
           ", \"cropKeepAspect\": " + (st.cropKeepAspect ? "true" : "false") + " },\n";
    out += "  \"video\": { \"path\": \"" + EscapeJson(video.path) +
           "\", \"loop\": " + (video.loop ? "true" : "false") + " },\n";
    out += "  \"camera\": { \"id\": \"" + EscapeJson(cam.id) +
           "\", \"name\": \"" + EscapeJson(cam.name) +
           "\", \"capture\": \"" + EscapeJson(cam.capture) + "\" },\n";
    out += "  \"quality\": \"" + EscapeJson(quality) + "\",\n";
    out += "  \"hotkey\": { \"modifiers\": " + std::to_string(hotkey.modifiers) +
           ", \"vk\": " + std::to_string(hotkey.vk) + " },\n";
    out += "  \"recordHotkey\": { \"modifiers\": " + std::to_string(recordHotkey.modifiers) +
           ", \"vk\": " + std::to_string(recordHotkey.vk) + " },\n";
    out += "  \"videoHotkey\": { \"modifiers\": " + std::to_string(videoHotkey.modifiers) +
           ", \"vk\": " + std::to_string(videoHotkey.vk) + " },\n";
    out += "  \"sourceStaticHotkey\": { \"modifiers\": " + std::to_string(sourceStaticHotkey.modifiers) +
           ", \"vk\": " + std::to_string(sourceStaticHotkey.vk) + " },\n";
    out += "  \"sourceVideoHotkey\": { \"modifiers\": " + std::to_string(sourceVideoHotkey.modifiers) +
           ", \"vk\": " + std::to_string(sourceVideoHotkey.vk) + " },\n";
    out += "  \"sourceCameraHotkey\": { \"modifiers\": " + std::to_string(sourceCameraHotkey.modifiers) +
           ", \"vk\": " + std::to_string(sourceCameraHotkey.vk) + " },\n";
    out += "  \"record\": { \"path\": \"" + EscapeJson(record.path) + "\" },\n";
    out += "  \"autostart\": ";
    out += autostart ? "true" : "false";
    out += "\n";
    out += "}\n";
    return out;
}

bool Settings::Save(const std::wstring& path) const
{
    std::wstring dir;
    size_t slash = path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) dir = path.substr(0, slash);
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr); // уже существует — норма
    return WriteUtf8FileNoBom(path, Serialize());
}

SourceConfig ToSourceConfig(const Settings& s, const std::wstring& type)
{
    SourceConfig cfg;
    cfg.type = type;
    if (type == L"video") {
        cfg.path = s.video.path;
        cfg.scaleMode = L"fit";
        cfg.loop = s.video.loop;
    } else if (type == L"camera") {
        cfg.path = s.cam.id;
        cfg.camName = s.cam.name; // scaleMode/crop не задаём — для camera не имеют смысла
        // Нормализация как в ParseCapture: только 720p/1080p проходят, остальное -> max.
        cfg.capture = (s.cam.capture == L"720p" || s.cam.capture == L"1080p")
                          ? s.cam.capture
                          : L"max";
    } else if (type == L"static") {
        cfg.path = s.st.path;
        cfg.scaleMode = s.st.scaleMode;
        cfg.cropX = s.st.cropX;
        cfg.cropY = s.st.cropY;
        cfg.cropW = s.st.cropW;
        cfg.cropH = s.st.cropH;
        cfg.cropKeepAspect = s.st.cropKeepAspect;
    }
    return cfg; // неизвестный тип: конфиг пуст, CreateSource вернёт nullptr -> fallback
}

SourceConfig ToSourceConfig(const Settings& s)
{
    return ToSourceConfig(s, s.sourceType);
}
