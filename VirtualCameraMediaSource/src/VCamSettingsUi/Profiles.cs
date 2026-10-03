using System.Diagnostics;

namespace VCamSettingsUi;

// Named settings snapshots (source + static/video/camera sections + effects +
// quality). Storage is a plain directory — %APPDATA%\VCam\profiles\*.json —
// where each file is an ordinary Settings serialization (Settings.Load/Save
// with an explicit path), so the C++ side needs no profile parser at all.
// Applying a profile (MainForm.ApplyProfile) writes ONLY its effects section
// + root quality into the live settings.json — source sections stay untouched,
// so the current source keeps streaming; the host picks the change up via
// hot-reload as usual. The profile display name is the file name
// without ".json" (Cyrillic names are fine on NTFS and share by plain copy).
public static class Profiles
{
    public static string DefaultDirectoryPath => Path.Combine(Settings.DirectoryPath, "profiles");

    // Built-in examples: embedded resource logical name -> profile name (file
    // name on disk = name + ".json" via PathFor/Sanitize). The JSON payloads
    // live in profiles-seed/*.json (EmbeddedResource in the csproj); Russian
    // display names are the file base names (shared with tests via identical
    // LogicalName entries in the temp round-trip project).
    public static readonly (string Resource, string FileName)[] Seeds =
    {
        ("profiles.seed.clean", "Чистый эфир"),
        ("profiles.seed.vhs", "VHS"),
        ("profiles.seed.retro-bw", "Ретро Ч/Б"),
        ("profiles.seed.mirror", "Зеркало"),
        ("profiles.seed.tracking", "Трекинг-хаос"),
    };

    // Copies the built-in examples on first launch. Never overwrites: every
    // seed is written only when its target file is missing, so user profiles
    // (including same-named edits) are never clobbered. Seed names go through
    // PathFor/Sanitize like any other name (e.g. "Ретро Ч/Б" lands on disk
    // with a fullwidth solidus).
    public static void EnsureSeeded(string? dir = null)
    {
        var target = dir ?? DefaultDirectoryPath;
        Directory.CreateDirectory(target);
        var asm = typeof(Profiles).Assembly;
        foreach (var (resource, fileName) in Seeds)
        {
            var path = PathFor(fileName, target);
            if (File.Exists(path)) continue;
            try
            {
                using var stream = asm.GetManifestResourceStream(resource);
                if (stream is null)
                {
                    Debug.WriteLine($"Profiles: seed resource missing: {resource}");
                    continue;
                }
                using var reader = new StreamReader(stream);
                var text = reader.ReadToEnd();
                // Validate before writing: a broken seed must never land on disk.
                var s = Settings.LoadFromText(text);
                s.Save(path);
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Profiles: seed {fileName} failed: {ex.Message}");
            }
        }
    }

    public static List<string> List(string? dir = null)
    {
        var target = dir ?? DefaultDirectoryPath;
        if (!Directory.Exists(target)) return new List<string>();
        return Directory.GetFiles(target, "*.json")
            .Select(Path.GetFileNameWithoutExtension)
            .Where(n => !string.IsNullOrWhiteSpace(n))
            .Cast<string>()
            .OrderBy(n => n, StringComparer.CurrentCulture)
            .ToList();
    }

    public static string PathFor(string name, string? dir = null) =>
        Path.Combine(dir ?? DefaultDirectoryPath, Sanitize(name) + ".json");

    public static Settings LoadProfile(string name, string? dir = null)
    {
        // LoadFromText throws on malformed JSON — the caller shows the error
        // instead of fail-softing to defaults (which would nuke the user's
        // live settings on apply).
        return Settings.LoadFromText(File.ReadAllText(PathFor(name, dir)));
    }

    public static void SaveProfile(string name, Settings settings, string? dir = null) =>
        settings.Save(PathFor(name, dir));

    public static void DeleteProfile(string name, string? dir = null) =>
        File.Delete(PathFor(name, dir));

    public static bool Exists(string name, string? dir = null) =>
        File.Exists(PathFor(name, dir));

    // Display names stay human ("Ретро Ч/Б"): the two directory separators
    // become a fullwidth solidus (U+FF0F, visually identical), every other
    // invalid character becomes '_'. Everything keys off the file name, so
    // List/Load/Save/Delete stay consistent for both spellings.
    public static string Sanitize(string name)
    {
        var invalid = Path.GetInvalidFileNameChars();
        var clean = new string((name ?? "").Trim()
            .Select(c => c == '/' || c == '\\' ? '／'
                : invalid.Contains(c) ? '_' : c).ToArray());
        while (clean.Contains("  ", StringComparison.Ordinal))
            clean = clean.Replace("  ", " ", StringComparison.Ordinal);
        clean = clean.Trim().TrimEnd('.');
        if (clean.Length > 80) clean = clean[..80].TrimEnd();
        return string.IsNullOrEmpty(clean) ? "Профиль" : clean;
    }
}
