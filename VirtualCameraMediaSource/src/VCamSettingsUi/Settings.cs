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
//     "effects": { "mirror": bool, "grayscale": bool, "noise": bool,
//                "scanlines": bool, "rgbsplit": bool, "tracking": bool,
//                "vhs": bool, "noiseLevel": int 0-100, "scanlinesLevel": int,
//                "rgbsplitLevel": int, "trackingLevel": int },
//     "autostart": bool }
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
public enum SourceType
{
    Static,
    Video,
    Camera,
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

    // Section "source".
    public SourceType SourceType { get; set; } = SourceType.Static;

    // Root "quality" (v2): mirrors Settings::ParseQuality on the C++ side —
    // only "fixed720p" passes, anything else (incl. missing) is Source.
    public Quality Quality { get; set; } = Quality.Source;

    // Section "effects" (host post-fx): mirrors EffectsSection on the C++ side.
    // Legacy files without the section migrate to all-false.
    // vhs = VHS preset (host ORs all four interferences at once).
    public bool FxMirror { get; set; }
    public bool FxGrayscale { get; set; }
    public bool FxNoise { get; set; }
    public bool FxScanlines { get; set; }
    public bool FxRgbSplit { get; set; }
    public bool FxTracking { get; set; }
    public bool FxVhs { get; set; }

    // Analog interference intensity 0-100 (mirrors EffectsSection levels on
    // the C++ side). Missing key -> 100, clamped on read. Level 0 with the
    // toggle on ~= effect off. VHS has no own level: it uses these four.
    public int FxNoiseLevel { get; set; } = 100;
    public int FxScanlinesLevel { get; set; } = 100;
    public int FxRgbSplitLevel { get; set; } = 100;
    public int FxTrackingLevel { get; set; } = 100;

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
                    // Unknown tokens map to Static for editing; the host keeps them verbatim.
                    var typeToken = type.GetString();
                    s.SourceType = string.Equals(typeToken, "video", StringComparison.OrdinalIgnoreCase)
                        ? SourceType.Video
                        : string.Equals(typeToken, "camera", StringComparison.OrdinalIgnoreCase)
                            ? SourceType.Camera
                            : SourceType.Static;
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

                if (root.TryGetProperty("effects", out var fx) && fx.ValueKind == JsonValueKind.Object)
                {
                    s.FxMirror = GetBool(fx, "mirror");
                    s.FxGrayscale = GetBool(fx, "grayscale");
                    s.FxNoise = GetBool(fx, "noise");
                    s.FxScanlines = GetBool(fx, "scanlines");
                    s.FxRgbSplit = GetBool(fx, "rgbsplit");
                    s.FxTracking = GetBool(fx, "tracking");
                    s.FxVhs = GetBool(fx, "vhs");
                    s.FxNoiseLevel = GetLevel(fx, "noiseLevel");
                    s.FxScanlinesLevel = GetLevel(fx, "scanlinesLevel");
                    s.FxRgbSplitLevel = GetLevel(fx, "rgbsplitLevel");
                    s.FxTrackingLevel = GetLevel(fx, "trackingLevel");
                }

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
                s.SourceType = isVideo ? SourceType.Video : SourceType.Static;
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
    public void Save(string? filePath = null)
    {
        var payload = new Dictionary<string, object>
        {
            ["source"] = new Dictionary<string, object>
            {
                ["type"] = SourceType switch
                {
                    SourceType.Video => "video",
                    SourceType.Camera => "camera",
                    _ => "static",
                },
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
            ["effects"] = new Dictionary<string, object>
            {
                ["mirror"] = FxMirror,
                ["grayscale"] = FxGrayscale,
                ["noise"] = FxNoise,
                ["scanlines"] = FxScanlines,
                ["rgbsplit"] = FxRgbSplit,
                ["tracking"] = FxTracking,
                ["vhs"] = FxVhs,
                ["noiseLevel"] = FxNoiseLevel,
                ["scanlinesLevel"] = FxScanlinesLevel,
                ["rgbsplitLevel"] = FxRgbSplitLevel,
                ["trackingLevel"] = FxTrackingLevel,
            },
            ["autostart"] = Autostart,
        };

        var path = filePath ?? FilePath;
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(path, JsonSerializer.Serialize(payload, SerializerOptions), new UTF8Encoding(false));
    }

    private static string GetString(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() ?? ""
            : "";

    private static int GetInt(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number && v.TryGetInt32(out var n)
            ? n
            : 0;

    private static bool GetBool(JsonElement obj, string name) =>
        obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;

    // Mirrors the C++ ClampLevel exactly: missing/non-numeric -> 100,
    // out-of-range -> clamp 0-100.
    private static int GetLevel(JsonElement obj, string name)
    {
        if (obj.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number &&
            v.TryGetInt32(out var n))
            return Math.Clamp(n, 0, 100);
        return 100;
    }

    private static ScaleMode ParseScaleMode(string mode) =>
        string.Equals(mode, "cover", StringComparison.OrdinalIgnoreCase) ? ScaleMode.Cover
        : string.Equals(mode, "crop", StringComparison.OrdinalIgnoreCase) ? ScaleMode.Crop
        : ScaleMode.Fit;

    // Mirrors the C++ ParseQuality exactly: only "fixed720p" passes (ordinal),
    // everything else (missing/garbage/future tokens) is Source.
    private static Quality ParseQuality(string quality) =>
        string.Equals(quality, "fixed720p", StringComparison.Ordinal) ? Quality.Fixed720p
        : Quality.Source;

    // Mirrors the C++ ParseCapture exactly: only "720p"/"1080p" pass (ordinal),
    // everything else (missing/garbage/future tokens) is Max.
    private static CaptureMode ParseCapture(string capture) =>
        string.Equals(capture, "720p", StringComparison.Ordinal) ? CaptureMode.P720
        : string.Equals(capture, "1080p", StringComparison.Ordinal) ? CaptureMode.P1080
        : CaptureMode.Max;
}
