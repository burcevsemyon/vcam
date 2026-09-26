using System.Text.Json;

namespace VCamSettingsUi;

// Flat settings file shared with StaticProducer.exe and VideoProducer.exe (C++).
// Contract: { "imagePath": "...", "scaleMode": "fit" | "cover" | "crop",
//            "cropX": int, "cropY": int, "cropW": int, "cropH": int,
//            "cropKeepAspect": bool,
//            "mediaMode": "static" | "video", "mediaPath": "..." }
// NOTE: field names and value tokens must stay in sync with
// src/StaticProducer/StaticProducer.cpp and src/VideoProducer/VideoProducer.cpp
// (JsonGetString/LoadSettings/EffectiveImagePath).
public enum ScaleMode
{
    Fit,
    Cover,
    Crop,
}

// Which provider feeds the camera: a still image (StaticProducer) or a video
// clip (VideoProducer). Back-compat: a file without "mediaMode" means Static.
public enum MediaMode
{
    Static,
    Video,
}

public sealed class Settings
{
    public string ImagePath { get; set; } = "";
    public ScaleMode ScaleMode { get; set; } = ScaleMode.Fit;

    // Crop rect in SOURCE image pixels; used only when ScaleMode == Crop.
    // Field names must stay in sync with src/StaticProducer/StaticProducer.cpp (JsonGetInt).
    public int CropX { get; set; }
    public int CropY { get; set; }
    public int CropW { get; set; }
    public int CropH { get; set; }

    // Crop mode: letterbox the region instead of stretching it to 1280x720.
    public bool CropKeepAspect { get; set; }

    // Media selection: MediaPath is the active source file for the current
    // MediaMode ("static" -> image, "video" -> clip). StaticProducer prefers
    // mediaPath when mediaMode != "video" (EffectiveImagePath), VideoProducer
    // prefers mediaPath over argv[1].
    public MediaMode MediaMode { get; set; } = MediaMode.Static;
    public string MediaPath { get; set; } = "";

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
            var s = new Settings();
            if (root.TryGetProperty("imagePath", out var p)) s.ImagePath = p.GetString() ?? "";
            if (root.TryGetProperty("scaleMode", out var m))
            {
                var mode = m.GetString();
                if (string.Equals(mode, "cover", StringComparison.OrdinalIgnoreCase)) s.ScaleMode = ScaleMode.Cover;
                else if (string.Equals(mode, "crop", StringComparison.OrdinalIgnoreCase)) s.ScaleMode = ScaleMode.Crop;
            }
            if (root.TryGetProperty("cropX", out var cx)) s.CropX = cx.GetInt32();
            if (root.TryGetProperty("cropY", out var cy)) s.CropY = cy.GetInt32();
            if (root.TryGetProperty("cropW", out var cw)) s.CropW = cw.GetInt32();
            if (root.TryGetProperty("cropH", out var ch)) s.CropH = ch.GetInt32();
            if (root.TryGetProperty("cropKeepAspect", out var ka) && ka.ValueKind == JsonValueKind.True) s.CropKeepAspect = true;
            // Back-compat: absent/unknown mediaMode -> Static, absent mediaPath -> "".
            if (root.TryGetProperty("mediaMode", out var mm) && mm.ValueKind == JsonValueKind.String)
            {
                var media = mm.GetString();
                s.MediaMode = string.Equals(media, "video", StringComparison.OrdinalIgnoreCase)
                    ? MediaMode.Video
                    : MediaMode.Static;
            }
            if (root.TryGetProperty("mediaPath", out var mp) && mp.ValueKind == JsonValueKind.String)
                s.MediaPath = mp.GetString() ?? "";
            return s;
        }
        catch
        {
            return new Settings();
        }
    }

    // Writer intentionally mirrors the C++ reader: ASCII keys, forward-slash-free JSON
    // is produced by JsonSerializer; the C++ side unescapes \\ and \".
    public void Save(string? filePath = null)
    {
        // Static mode mirrors EffectiveImagePath (StaticProducer.cpp): mediaPath is
        // the source, so imagePath is kept in sync - an old StaticProducer that only
        // reads imagePath still picks the same file. In video mode imagePath is left
        // exactly as loaded (StaticProducer falls back to it while mediaMode == video).
        var imagePath = ImagePath;
        if (MediaMode == MediaMode.Static && !string.IsNullOrEmpty(MediaPath))
            imagePath = MediaPath;

        var payload = new Dictionary<string, object>
        {
            ["imagePath"] = imagePath,
            ["mediaMode"] = MediaMode == MediaMode.Video ? "video" : "static",
            ["mediaPath"] = MediaPath,
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
        };

        var path = filePath ?? FilePath;
        var dir = Path.GetDirectoryName(path);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        File.WriteAllText(path, JsonSerializer.Serialize(payload, new JsonSerializerOptions
        {
            WriteIndented = true,
        }));
    }
}
