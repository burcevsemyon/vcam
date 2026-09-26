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
//     "camera": { "id": "<MF symbolic link>", "name": "<friendly name>" },
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
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
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
        catch
        {
            return new Settings();
        }
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

    private static ScaleMode ParseScaleMode(string mode) =>
        string.Equals(mode, "cover", StringComparison.OrdinalIgnoreCase) ? ScaleMode.Cover
        : string.Equals(mode, "crop", StringComparison.OrdinalIgnoreCase) ? ScaleMode.Crop
        : ScaleMode.Fit;
}
