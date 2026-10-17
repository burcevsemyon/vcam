using VCamSettingsUi;
using Xunit;

namespace VCamUiTests;

// Контракт video.loop + videoHotkey C# <-> C++ (VideoSection/VideoHotkeySection):
// round-trip и дефолты (loop=false = один проход, хоткей Ctrl+Alt+P).
// Без запуска UI (test-seam filePath).
public sealed class SettingsVideoTests : IDisposable
{
    private readonly string _file = Path.Combine(
        Path.GetTempPath(), "VCamUiTests", $"v_{Guid.NewGuid():N}.json");

    public void Dispose()
    {
        try { if (File.Exists(_file)) File.Delete(_file); } catch { /* best-effort */ }
        GC.SuppressFinalize(this);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void RoundTrip_preserves_video_loop(bool loop)
    {
        var s = new Settings { VideoPath = @"C:\v\clip.mp4", VideoLoop = loop };
        s.Save(_file);
        Assert.Contains($"\"loop\": {(loop ? "true" : "false")}",
            File.ReadAllText(_file), StringComparison.Ordinal);
        Assert.Equal(loop, Settings.Load(_file).VideoLoop);
    }

    [Fact]
    public void Missing_loop_defaults_to_false()
    {
        File.WriteAllText(_file, "{\"video\": {\"path\": \"a.mp4\"}}");
        Assert.False(Settings.Load(_file).VideoLoop);
    }

    [Fact]
    public void Defaults_are_no_loop_and_ctrl_alt_p()
    {
        var s = new Settings();
        Assert.False(s.VideoLoop);
        Assert.Equal(3, s.VideoHotkeyModifiers);
        Assert.Equal(0x50, s.VideoHotkeyVk);
    }

    [Fact]
    public void RoundTrip_preserves_video_hotkey()
    {
        var s = new Settings { VideoHotkeyModifiers = 6, VideoHotkeyVk = 0x41 };
        s.Save(_file);
        var r = Settings.Load(_file);
        Assert.Equal(6, r.VideoHotkeyModifiers);
        Assert.Equal(0x41, r.VideoHotkeyVk);
    }

    [Theory]
    [InlineData(0)]     // мусор -> default
    [InlineData(16)]    // вне 1-15 -> default
    [InlineData(7)]     // проходит
    public void Video_hotkey_modifiers_clamped(int mods)
    {
        File.WriteAllText(_file,
            $"{{\"static\": {{}}, \"videoHotkey\": {{\"modifiers\": {mods}, \"vk\": 80}}}}");
        var expected = mods >= 1 && mods <= 15 ? mods : 3;
        Assert.Equal(expected, Settings.Load(_file).VideoHotkeyModifiers);
    }

    [Theory]
    [InlineData(0)]      // мусор -> default 0x50
    [InlineData(255)]    // вне 0x08-0xFE -> default
    [InlineData(8)]      // проходит
    public void Video_hotkey_vk_clamped(int vk)
    {
        File.WriteAllText(_file,
            $"{{\"static\": {{}}, \"videoHotkey\": {{\"modifiers\": 3, \"vk\": {vk}}}}}");
        var expected = vk >= 0x08 && vk <= 0xFE ? vk : 0x50;
        Assert.Equal(expected, Settings.Load(_file).VideoHotkeyVk);
    }
}
