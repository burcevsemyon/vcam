using VCamSettingsUi;
using Xunit;

namespace VCamUiTests;

// Контракт sourceStaticHotkey/sourceVideoHotkey/sourceCameraHotkey C# <-> C++
// (SourceSwitchHotkeySection): round-trip, дефолты Ctrl+Alt+1/2/3, границы.
// Без запуска UI (test-seam filePath).
public sealed class SettingsSourceHotkeyTests : IDisposable
{
    private readonly string _file = Path.Combine(
        Path.GetTempPath(), "VCamUiTests", $"sh_{Guid.NewGuid():N}.json");

    public void Dispose()
    {
        try { if (File.Exists(_file)) File.Delete(_file); } catch { /* best-effort */ }
        GC.SuppressFinalize(this);
    }

    [Fact]
    public void Defaults_are_ctrl_alt_1_2_3()
    {
        var s = new Settings();
        Assert.Equal(3, s.SourceStaticHotkeyModifiers);
        Assert.Equal(0x31, s.SourceStaticHotkeyVk);
        Assert.Equal(3, s.SourceVideoHotkeyModifiers);
        Assert.Equal(0x32, s.SourceVideoHotkeyVk);
        Assert.Equal(3, s.SourceCameraHotkeyModifiers);
        Assert.Equal(0x33, s.SourceCameraHotkeyVk);
    }

    [Fact]
    public void RoundTrip_preserves_source_hotkeys()
    {
        var s = new Settings
        {
            SourceStaticHotkeyModifiers = 6, SourceStaticHotkeyVk = 0x41,
            SourceVideoHotkeyModifiers = 7, SourceVideoHotkeyVk = 0x42,
            SourceCameraHotkeyModifiers = 5, SourceCameraHotkeyVk = 0x43,
        };
        s.Save(_file);
        var r = Settings.Load(_file);
        Assert.Equal(6, r.SourceStaticHotkeyModifiers);
        Assert.Equal(0x41, r.SourceStaticHotkeyVk);
        Assert.Equal(7, r.SourceVideoHotkeyModifiers);
        Assert.Equal(0x42, r.SourceVideoHotkeyVk);
        Assert.Equal(5, r.SourceCameraHotkeyModifiers);
        Assert.Equal(0x43, r.SourceCameraHotkeyVk);
    }

    [Fact]
    public void Missing_sections_keep_defaults()
    {
        File.WriteAllText(_file, "{\"source\": {\"type\": \"static\"}}");
        var s = Settings.Load(_file);
        Assert.Equal(0x31, s.SourceStaticHotkeyVk);
        Assert.Equal(0x32, s.SourceVideoHotkeyVk);
        Assert.Equal(0x33, s.SourceCameraHotkeyVk);
    }

    [Fact]
    public void Vk_clamped_to_per_section_default()
    {
        File.WriteAllText(_file,
            "{\"static\": {}, \"sourceStaticHotkey\": {\"modifiers\": 3, \"vk\": 0}, " +
            "\"sourceVideoHotkey\": {\"modifiers\": 3, \"vk\": 255}, " +
            "\"sourceCameraHotkey\": {\"modifiers\": 3, \"vk\": 8}}");
        var s = Settings.Load(_file);
        Assert.Equal(0x31, s.SourceStaticHotkeyVk); // 0 -> Ctrl+Alt+1
        Assert.Equal(0x32, s.SourceVideoHotkeyVk);  // 255 -> Ctrl+Alt+2
        Assert.Equal(8, s.SourceCameraHotkeyVk);    // 8 -> pass
    }

    [Theory]
    [InlineData(0)]
    [InlineData(16)]
    [InlineData(7)]
    public void Modifiers_clamped(int mods)
    {
        File.WriteAllText(_file,
            $"{{\"static\": {{}}, \"sourceVideoHotkey\": {{\"modifiers\": {mods}, \"vk\": 50}}}}");
        var expected = mods >= 1 && mods <= 15 ? mods : 3;
        Assert.Equal(expected, Settings.Load(_file).SourceVideoHotkeyModifiers);
    }
}
