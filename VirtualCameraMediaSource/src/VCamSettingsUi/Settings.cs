using System.Text.Json;

namespace VCamSettingsUi;

// Flat settings file shared with StaticProducer.exe (C++).
// Contract: { "imagePath": "...", "scaleMode": "fit" | "cover" }
// NOTE: field names and value tokens must stay in sync with
// src/StaticProducer/StaticProducer.cpp (JsonGetString/LoadSettings).
public enum ScaleMode
{
    Fit,
    Cover,
}

public sealed class Settings
{
    public string ImagePath { get; set; } = "";
    public ScaleMode ScaleMode { get; set; } = ScaleMode.Fit;

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
            if (root.TryGetProperty("scaleMode", out var m) &&
                string.Equals(m.GetString(), "cover", StringComparison.OrdinalIgnoreCase))
            {
                s.ScaleMode = ScaleMode.Cover;
            }
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
        var payload = new Dictionary<string, string>
        {
            ["imagePath"] = ImagePath,
            ["scaleMode"] = ScaleMode == ScaleMode.Cover ? "cover" : "fit",
        };
        File.WriteAllText(FilePath, JsonSerializer.Serialize(payload, new JsonSerializerOptions
        {
            WriteIndented = true,
        }));
    }
}
