using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace VCamSettingsUi;

// Settings file shared with the C++ side (src/ProducerCore/Settings.cpp).
// Contract - NEW schema (only format that is written):
//   { "source": { "type": "static" | "video" | "camera" },
//     "static": { "path": "...", "scaleMode": "fit" | "cover" | "crop",
//                 "cropX": int, "cropY": int, "cropW": int, "cropH": int,
//                 "cropKeepAspect": bool },
//     "video":  { "path": "..." },
//     "camera": { "id": "<MF symbolic link>", "name": "<friendly name>",
///                "capture": "max" | "720p" | "1080p" },
//     "quality": "source" | "fixed720p",
//     "hotkey": { "modifiers": int 1-15, "vk": int 0x08-0xFE },
//     "recordHotkey": { "modifiers": int 1-15, "vk": int 0x08-0xFE },
//     "record": { "path": "..." },
//     "autostart": bool }
// Unknown sections (e.g. legacy "effects" from older versions) are ignored
// on read and dropped on save — old files never fail to load.
// Empty camera section (id and name both "") -> host shows NO SIGNAL until a
// device is chosen. Load also accepts the legacy flat format
// (imagePath/mediaMode/mediaPath/scaleMode/crop*) and migrates it with the
// same rules as the C++ loader (the legacy format has no camera section).
// NOTE: field names and value tokens must stay in sync with
// src/ProducerCore/Settings.cpp (ParseNewSchema/ParseLegacySchema/Serialize).
public enum ScaleMode
{
    Fit,
    Cover,
    Crop,
}

// Which section feeds the camera: source.type in settings.json.
// В3: raw-строка (C++ хранит токен verbatim), хост по неизвестному уходит в
// verbatim (Settings.cpp ParseNewSchema), хост по неизвестному уходит в
// fallback, но токен живёт. Известные — канон нижнего регистра
// (insensitive как раньше); будущие неизвестные — verbatim обратно в Save,
// чтобы текущий UI их не схлопывал в "static". Пусто = "static" (как C++).
public static class SourceTypes
{
    public const string Static = "static";
    public const string Video = "video";
    public const string Camera = "camera";

    public static bool IsKnown(string? token) =>
        string.Equals(token, Static, StringComparison.OrdinalIgnoreCase) ||
        string.Equals(token, Video, StringComparison.OrdinalIgnoreCase) ||
        string.Equals(token, Camera, StringComparison.OrdinalIgnoreCase);
}

// v2 frame quality (phase vcam-quality-v2): Source = native size of the
// active source (default; legacy files without the key migrate to this),
// Fixed720p = v2 mirrors 720p (ladder only downwards).
public enum Quality
{
    Source,
    Fixed720p,
}

// Physical capture height (phase vcam-camera-capture-size): Max = best
// within the native cap (default; legacy files without the key migrate to
// this), P720/P1080 = prefer a native mode of that height (nearest on miss).
public enum CaptureMode
{
    Max,
    P720,
    P1080,
}

public sealed class Settings
{
    // Section "static" (still image): crop rect is in SOURCE image pixels and is
    // used only when ScaleMode == Crop.
    public string StaticPath { get; set; } = "";
    public ScaleMode ScaleMode { get; set; } = ScaleMode.Fit;
    public int CropX { get; set; }
    public int CropY { get; set; }
    public int CropW { get; set; }
    public int CropH { get; set; }
    public bool CropKeepAspect { get; set; }

    // Section "video" (clip).
    public string VideoPath { get; set; } = "";

    // Section "camera" (physical webcam device): id = MF symbolic link
    // (stable per USB port), name = friendly name. Both empty = not chosen.
    public string CameraId { get; set; } = "";
    public string CameraName { get; set; } = "";

    // Section "source": raw token, see SourceTypes. UI maps unknown future
    // tokens to the static view, but saves them back verbatim (MainForm keeps
    // the token while the media combo is untouched by the user).
    public string SourceType { get; set; } = SourceTypes.Static;

    // Root "quality" (v2): mirrors Settings::ParseQuality on the C++ side —
    // only "fixed720p" passes, anything else (incl. missing) is Source.
    public Quality Quality { get; set; } = Quality.Source;

    // Section "hotkey" (global host hotkey static->video->auto-static): mirrors
    // HotkeySection on the C++ side. Modifiers are RegisterHotKey bits
    // (MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=4, MOD_WIN=8); vk is the Virtual-Key
    // code. Missing key or garbage (incl. 0) -> default Ctrl+Alt+V (3, 0x56).
    public int HotkeyModifiers { get; set; } = 3;
    public int HotkeyVk { get; set; } = 0x56;

    // Section "recordHotkey" (host record start/stop toggle): mirrors
    // RecordHotkeySection on the C++ side. Same rules as hotkey, default
    // Ctrl+Alt+R (3, 0x52).
    public int RecordHotkeyModifiers { get; set; } = 3;
    public int RecordHotkeyVk { get; set; } = 0x52;

    // Section "record" (default output path for the ether recording): mirrors
    // RecordSection on the C++ side. Empty = the host generates
    // %USERPROFILE%\Videos\VCam_yyyyMMdd_HHmmss.mp4 at record start.
    // The recording STATE (on/off) never lives here — only the transient
    // %APPDATA%\VCam\record_state.json the host writes while recording.
    public string RecordPath { get; set; } = "";

    // Section "camera" capture: mirrors Settings::ParseCapture on the C++ side —
    // only "720p"/"1080p" pass, anything else (incl. missing) is Max.
    public CaptureMode Capture { get; set; } = CaptureMode.Max;

    // Root: single source of truth for the HKCU Run entry (host + UI).
    public bool Autostart { get; set; } = true;

    private static readonly JsonSerializerOptions SerializerOptions = new()
    {
        WriteIndented = true,
        // Keep non-ASCII (Cyrillic paths) verbatim: the C++ reader decodes \uXXXX
        // as '?', so escaping them would corrupt the path.
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
    };

    public static string DirectoryPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "VCam");

    // filePath: optional override so round-trip tests can use a temp file
    // instead of %APPDATA%\VCam\settings.json. Production callers pass nothing.
    public static string FilePath => Path.Combine(DirectoryPath, "settings.json");

    public static Settings Load(string? filePath = null)
    {
        try
        {
            var path = filePath ?? FilePath;
            if (!File.Exists(path)) return new Settings();
            return LoadFromText(File.ReadAllText(path));
        }
        catch
        {
            return new Settings();
        }
    }

    // Parses an already-read snapshot (throws on malformed JSON — the caller
    // decides between a silent default and a retry). Load() above keeps the
    // old fail-soft contract; the live-sync watcher uses this directly so it
    // applies exactly the validated text it has just read (no TOCTOU re-read).
    public static Settings LoadFromText(string text)
    {
        using var doc = JsonDocument.Parse(text);
        var root = doc.RootElement;
        if (root.ValueKind != JsonValueKind.Object) return new Settings();
        var s = new Settings();

            var isNew = root.TryGetProperty("source", out _) ||
                        root.TryGetProperty("static", out _) ||
                        root.TryGetProperty("video", out _) ||
                        root.TryGetProperty("camera", out _);

            if (isNew)
            {
                if (root.TryGetProperty("source", out var src) && src.ValueKind == JsonValueKind.Object &&
                    src.TryGetProperty("type", out var type) && type.ValueKind == JsonValueKind.String)
                {
                    // В3: raw-строка как C++ (verbatim для будущих типов).
                    s.SourceType = NormalizeSourceType(type.GetString() ?? "");
                }

                if (root.TryGetProperty("static", out var st) && st.ValueKind == JsonValueKind.Object)
                {
                    s.StaticPath = GetString(st, "path");
                    s.ScaleMode = ParseScaleMode(GetString(st, "scaleMode"));
                    s.CropX = GetInt(st, "cropX");
                    s.CropY = GetInt(st, "cropY");
                    s.CropW = GetInt(st, "cropW");
                    s.CropH = GetInt(st, "cropH");
                    s.CropKeepAspect = GetBool(st, "cropKeepAspect");
                }

                if (root.TryGetProperty("video", out var vd) && vd.ValueKind == JsonValueKind.Object)
                    s.VideoPath = GetString(vd, "path");

                if (root.TryGetProperty("camera", out var cm) && cm.ValueKind == JsonValueKind.Object)
                {
                    s.CameraId = GetString(cm, "id");
                    s.CameraName = GetString(cm, "name");
                    s.Capture = ParseCapture(GetString(cm, "capture"));
                }

                if (root.TryGetProperty("quality", out var q) && q.ValueKind == JsonValueKind.String)
                    s.Quality = ParseQuality(q.GetString() ?? "");

                if (root.TryGetProperty("hotkey", out var hk) && hk.ValueKind == JsonValueKind.Object)
                {
                    s.HotkeyModifiers = ParseHotkeyModifiers(GetInt(hk, "modifiers", 3));
                    s.HotkeyVk = ParseHotkeyVk(GetInt(hk, "vk", 0x56));
                }

                if (root.TryGetProperty("recordHotkey", out var rhk) && rhk.ValueKind == JsonValueKind.Object)
                {
                    s.RecordHotkeyModifiers = ParseHotkeyModifiers(GetInt(rhk, "modifiers", 3));
                    s.RecordHotkeyVk = ParseRecordHotkeyVk(GetInt(rhk, "vk", 0x52));
                }

                if (root.TryGetProperty("record", out var rc) && rc.ValueKind == JsonValueKind.Object)
                    s.RecordPath = GetString(rc, "path");

                // Legacy "effects" section (older versions): ignored — old
                // files load without errors, a later Save drops the section.

                if (root.TryGetProperty("autostart", out var au))
                    s.Autostart = au.ValueKind != JsonValueKind.False;
            }
            else
            {
                // Legacy flat format - same migration rules as ProducerCore:
                // imagePath -> static.path, scaleMode/crop* -> static section,
                // mediaPath -> video.path, mediaMode=="video" -> source.type=video.
                var mediaMode = GetString(root, "mediaMode");
                var isVideo = string.Equals(mediaMode, "video", StringComparison.OrdinalIgnoreCase);
                s.SourceType = isVideo ? SourceTypes.Video : SourceTypes.Static;
                s.StaticPath = GetString(root, "imagePath");
                if (string.IsNullOrEmpty(s.StaticPath) && !isVideo)
                    s.StaticPath = GetString(root, "mediaPath");
                s.VideoPath = GetString(root, "mediaPath");
                s.ScaleMode = ParseScaleMode(GetString(root, "scaleMode"));
                s.CropX = GetInt(root, "cropX");
                s.CropY = GetInt(root, "cropY");
                s.CropW = GetInt(root, "cropW");
                s.CropH = GetInt(root, "cropH");
                s.CropKeepAspect = GetBool(root, "cropKeepAspect");
                if (root.TryGetProperty("autostart", out var au))
                    s.Autostart = au.ValueKind != JsonValueKind.False;
            }

            return s;
    }

    // Writes ONLY the new schema, UTF-8 without BOM (see SerializerOptions).
    // В6: атомарно через tmp+move (как host WriteRecordCommand): конкурентный
    // читатель (хост) видит либо старый, либо новый файл целиком — рваного
    // truncate-read нет. Плюс retry при sharing-violation (хост в этот момент
    // пишет свою копию).
    public void Save(string? filePath = null)
    {
        var payload = new Dictionary<string, object>
        {
            ["source"] = new Dictionary<string, object>
            {
                // В3: пишем токен verbatim (неизвестный будущий переживает
                // round-trip как в C++); пусто = "static".
                ["type"] = string.IsNullOrEmpty(SourceType) ? SourceTypes.Static : SourceType,
            },
            ["static"] = new Dictionary<string, object>
            {
                ["path"] = StaticPath,
                ["scaleMode"] = ScaleMode switch
                {
                    ScaleMode.Cover => "cover",
                    ScaleMode.Crop => "crop",
                    _ => "fit",
                },
                ["cropX"] = CropX,
                ["cropY"] = CropY,
                ["cropW"] = CropW,
                ["cropH"] = CropH,
                ["cropKeepAspect"] = CropKeepAspect,
            },
            ["video"] = new Dictionary<string, object>
            {
                ["path"] = VideoPath,
            },
            ["camera"] = new Dictionary<string, object>
            {
                ["id"] = CameraId,
                ["name"] = CameraName,
                ["capture"] = Capture switch
                {
                    CaptureMode.P720 => "720p",
                    CaptureMode.P1080 => "1080p",
                    _ => "max",
                },
            },
            ["quality"] = Quality == Quality.Fixed720p ? "fixed720p" : "source",
            ["hotkey"] = new Dictionary<string, object>
            {
                ["modifiers"] = HotkeyModifiers,
                ["vk"] = HotkeyVk,
            },
            ["recordHotkey"] = new Dictionary<string, object>
            {
                ["modifiers"] = RecordHotkeyModifiers,
                ["vk"] = RecordHotkeyVk,
            },
            ["record"] = new Dictionary<string, object>
            {
                ["path"] = RecordPath,
            },
            ["autostart"] = Autostart,
        };

        var path = filePath ?? FilePath;
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var text = JsonSerializer.Serialize(payload, SerializerOptions);
        // Уникальный tmp на запись (UI vs host пишут одновременно): общий
        // фиксированный tmp сталкивал писателей sharing-violation.
        var tmp = Path.Combine(dir ?? "", Path.GetRandomFileName());
        try
        {
            File.WriteAllText(tmp, text, new UTF8Encoding(false));
            for (var attempt = 0; ; attempt++)
            {
                try
                {
                    File.Move(tmp, path, true);
                    return;
                }
                // Конкурентный move второго писателя surfaces как
                // UnauthorizedAccess (а не IOException) — тоже retry.
                catch (Exception ex) when ((ex is IOException || ex is UnauthorizedAccessException) && attempt < 3)
                {
                    Thread.Sleep(50);
                }
            }
        }
        finally
        {
            try { if (File.Exists(tmp)) File.Delete(tmp); } catch { /* best-effort */ }
        }
    }

    private static string GetString(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() ?? ""
            : "";

    private static int GetInt(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n)
            ? n
            : 0;

    private static int GetInt(JsonElement obj, string name, int defaultValue) =>
        obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n)
            ? n
            : defaultValue;

    private static bool GetBool(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;

    // В3: известные токены — канон нижнего регистра (insensitive, как раньше
    // и как C++ K4-нормализация scaleMode); неизвестные будущие — verbatim;
    // пусто/нет — "static" (как C++ fallback).
    private static string NormalizeSourceType(string token)
    {
        if (string.Equals(token, SourceTypes.Video, StringComparison.OrdinalIgnoreCase))
            return SourceTypes.Video;
        if (string.Equals(token, SourceTypes.Camera, StringComparison.OrdinalIgnoreCase))
            return SourceTypes.Camera;
        if (string.Equals(token, SourceTypes.Static, StringComparison.OrdinalIgnoreCase) ||
            string.IsNullOrEmpty(token))
            return SourceTypes.Static;
        return token;
    }

    // К4: insensitive как C++ ParseScaleMode/EqCI (обе стороны) — "FIT"/"Cover"
    // дают тот же режим в UI и хосте; канон — нижний регистр.
    private static ScaleMode ParseScaleMode(string mode) =>
        string.Equals(mode, "cover", StringComparison.OrdinalIgnoreCase) ? ScaleMode.Cover
        : string.Equals(mode, "crop", StringComparison.OrdinalIgnoreCase) ? ScaleMode.Crop
        : ScaleMode.Fit;

    // Mirrors the C++ ParseQuality exactly: only "fixed720p" passes (ordinal),
    // everything else (missing/garbage/future tokens) is Source.
    private static Quality ParseQuality(string quality) =>
        string.Equals(quality, "fixed720p", StringComparison.Ordinal) ? Quality.Fixed720p
        : Quality.Source;

    // Mirrors the C++ hotkey parsing exactly: modifiers 1-15 pass, vk
    // 0x08-0xFE passes, anything else (missing/garbage/0) is Ctrl+Alt+V.
    private static int ParseHotkeyModifiers(int mods) =>
        mods >= 1 && mods <= 15 ? mods : 3;

    private static int ParseHotkeyVk(int vk) =>
        vk >= 0x08 && vk <= 0xFE ? vk : 0x56;

    // Mirrors the C++ RecordHotkeySection parsing exactly: same ranges,
    // default Ctrl+Alt+R (0x52) instead of Ctrl+Alt+V.
    private static int ParseRecordHotkeyVk(int vk) =>
        vk >= 0x08 && vk <= 0xFE ? vk : 0x52;

    // Mirrors the C++ ParseCapture exactly: only "720p"/"1080p" pass (ordinal),
    // everything else (missing/garbage/future tokens) is Max.
    private static CaptureMode ParseCapture(string capture) =>
        string.Equals(capture, "720p", StringComparison.Ordinal) ? CaptureMode.P720
        : string.Equals(capture, "1080p", StringComparison.Ordinal) ? CaptureMode.P1080
        : CaptureMode.Max;
}
