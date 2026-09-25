using System.Text.Json;

namespace VCamSettingsUi;

// Flat settings file shared with StaticProducer.exe (C++).
// Contract: { "imagePath": "...", "scaleMode": "fit" | "cover" | "crop",
//            "cropX": int, "cropY": int, "cropW": int, "cropH": int,
//            "cropKeepAspect": bool }
// NOTE: field names and value tokens must stay in sync with
// src/StaticProducer/StaticProducer.cpp (JsonGetString/LoadSettings).
public enum ScaleMode
{
    Fit,
    Cover,
    Crop,
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

    public static string DirectoryPath =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData), "VCam");

    public static string FilePath => Path.Combine(DirectoryPath, "settings.json");

    public static Settings Load()
    {
        try
        {
            if (!File.Exists(FilePath)) return new Settings();
            using var doc = JsonDocument.Parse(File.ReadAllText(FilePath));
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
            return s;
        }
        catch
        {
            return new Settings();
        }
    }

    // Writer intentionally mirrors the C++ reader: ASCII keys, forward-slash-free JSON
    // is produced by JsonSerializer; the C++ side unescapes \\ and \".
    public void Save()
    {
        Directory.CreateDirectory(DirectoryPath);
        var payload = new Dictionary<string, object>
        {
            ["imagePath"] = ImagePath,
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
        File.WriteAllText(FilePath, JsonSerializer.Serialize(payload, new JsonSerializerOptions
        {
            WriteIndented = true,
        }));
    }
}
