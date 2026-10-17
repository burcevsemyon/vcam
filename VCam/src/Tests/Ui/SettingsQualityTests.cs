using VCamSettingsUi;
using Xunit;

namespace VCamUiTests;

// Контракт quality C# <-> C++ (Settings::ParseQuality): токены фиксированы,
// мусор/отсутствие -> Source. Без запуска UI (test-seam filePath).
public sealed class SettingsQualityTests : IDisposable
{
    private readonly string _file = Path.Combine(
        Path.GetTempPath(), "VCamUiTests", $"q_{Guid.NewGuid():N}.json");

    public void Dispose()
    {
        try { if (File.Exists(_file)) File.Delete(_file); } catch { /* best-effort */ }
        GC.SuppressFinalize(this);
    }

    [Theory]
    [InlineData(Quality.Source, "source")]
    [InlineData(Quality.Fixed1080p, "fixed1080p")]
    [InlineData(Quality.Fixed720p, "fixed720p")]
    public void RoundTrip_preserves_token(Quality quality, string token)
    {
        var s = new Settings { Quality = quality };
        s.Save(_file);
        Assert.Contains($"\"quality\": \"{token}\"", File.ReadAllText(_file));
        Assert.Equal(quality, Settings.Load(_file).Quality);
    }

    [Theory]
    [InlineData("bogus")]
    [InlineData("FIXED720P")] // ordinal: регистр значим, как в C++ ParseQuality
    [InlineData("")]
    public void Garbage_or_empty_falls_back_to_source(string token)
    {
        // Секция static включает новую схему — тестируем именно ParseQuality,
        // а не legacy-дефолт.
        File.WriteAllText(_file, $"{{\"static\": {{}}, \"quality\": \"{token}\"}}");
        Assert.Equal(Quality.Source, Settings.Load(_file).Quality);
    }

    [Fact]
    public void Missing_key_falls_back_to_source()
    {
        File.WriteAllText(_file, "{}");
        Assert.Equal(Quality.Source, Settings.Load(_file).Quality);
    }
}
