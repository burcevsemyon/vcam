#include "Settings.h"

#include <windows.h>
#include <atlbase.h>

#include <cstdio>
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

bool WriteUtf8FileNoBom(const std::wstring& path, const std::string& content)
{
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    DWORD written = 0;
    BOOL ok = WriteFile(h, content.data(), (DWORD)content.size(), &written, nullptr);
    return ok && written == content.size();
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

bool ParseScaleMode(const std::wstring& m, std::wstring& out)
{
    if (m == L"fit" || m == L"cover" || m == L"crop") { out = m; return true; }
    out = L"fit";
    return false;
}

// quality: только source | fixed720p, остальное (включая будущий cap) -> source.
void ParseQuality(const std::wstring& q, std::wstring& out)
{
    out = (q == L"fixed720p") ? L"fixed720p" : L"source";
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
        JsonGetString(json.substr(b, e - b), "path", s.video.path);
    }
    if (FindObjectRange(json, "camera", b, e)) {
        std::string sec = json.substr(b, e - b);
        JsonGetString(sec, "id", s.cam.id);
        JsonGetString(sec, "name", s.cam.name);
        std::wstring cap;
        if (JsonGetString(sec, "capture", cap)) ParseCapture(cap, s.cam.capture);
    }
    if (FindObjectRange(json, "effects", b, e)) {
        std::string sec = json.substr(b, e - b);
        s.fx.mirror = JsonGetBool(sec, "mirror", false);
        s.fx.grayscale = JsonGetBool(sec, "grayscale", false);
        s.fx.noise = JsonGetBool(sec, "noise", false);
        s.fx.scanlines = JsonGetBool(sec, "scanlines", false);
        s.fx.rgbSplit = JsonGetBool(sec, "rgbsplit", false);
        s.fx.tracking = JsonGetBool(sec, "tracking", false);
        s.fx.vhs = JsonGetBool(sec, "vhs", false);
    }
    std::wstring q;
    if (JsonGetString(json, "quality", q)) ParseQuality(q, s.quality);
    s.autostart = JsonGetBool(json, "autostart", true);
}

// Старый формат: корневые imagePath/mediaMode/mediaPath/scaleMode/crop*.
void ParseLegacySchema(const std::string& json, Settings& s)
{
    std::wstring imagePath, mediaPath, mediaMode;
    JsonGetString(json, "imagePath", imagePath);
    JsonGetString(json, "mediaPath", mediaPath);
    JsonGetString(json, "mediaMode", mediaMode);

    s.sourceType = (mediaMode == L"video") ? L"video" : L"static";
    s.st.path = imagePath;
    if (s.st.path.empty() && mediaMode != L"video") s.st.path = mediaPath;

    std::wstring sm;
    if (JsonGetString(json, "scaleMode", sm)) ParseScaleMode(sm, s.st.scaleMode);
    s.st.cropX = JsonGetInt(json, "cropX", 0);
    s.st.cropY = JsonGetInt(json, "cropY", 0);
    s.st.cropW = JsonGetInt(json, "cropW", 0);
    s.st.cropH = JsonGetInt(json, "cropH", 0);
    s.st.cropKeepAspect = JsonGetBool(json, "cropKeepAspect", false);

    s.video.path = mediaPath;
    s.quality = L"source"; // legacy без ключа quality -> source
    s.cam.capture = L"max"; // legacy без секции camera -> max
    s.fx.mirror = false;    // legacy без секции effects -> всё выкл
    s.fx.grayscale = false;
    s.fx.noise = false;
    s.fx.scanlines = false;
    s.fx.rgbSplit = false;
    s.fx.tracking = false;
    s.fx.vhs = false;
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
    out += "  \"video\": { \"path\": \"" + EscapeJson(video.path) + "\" },\n";
    out += "  \"camera\": { \"id\": \"" + EscapeJson(cam.id) +
           "\", \"name\": \"" + EscapeJson(cam.name) +
           "\", \"capture\": \"" + EscapeJson(cam.capture) + "\" },\n";
    out += "  \"quality\": \"" + EscapeJson(quality) + "\",\n";
    out += "  \"effects\": { \"mirror\": ";
    out += fx.mirror ? "true" : "false";
    out += ", \"grayscale\": ";
    out += fx.grayscale ? "true" : "false";
    out += ", \"noise\": ";
    out += fx.noise ? "true" : "false";
    out += ", \"scanlines\": ";
    out += fx.scanlines ? "true" : "false";
    out += ", \"rgbsplit\": ";
    out += fx.rgbSplit ? "true" : "false";
    out += ", \"tracking\": ";
    out += fx.tracking ? "true" : "false";
    out += ", \"vhs\": ";
    out += fx.vhs ? "true" : "false";
    out += " },\n";
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
