using System.Diagnostics;
using System.Drawing;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using FlaUI.Core;
using FlaUI.Core.AutomationElements;
using FlaUI.Core.Definitions;
using FlaUI.UIA2;
using Xunit;

namespace VCamUiTests;

// Общая фикстура UIA-сценариев (xUnit collection fixture): один запуск формы
// на все сценарии коллекции. На время прогона подменяет settings.json
// тестовым (static + сгенерированный BMP 800x600) и возвращает оригинал
// байт-в-байт — как e2e_test.ps1. Живой хост при этом на секунды переключит
// эфир на тестовую картинку и вернётся обратно после restore.
[CollectionDefinition("UiApp")]
public sealed class UiAppCollection : ICollectionFixture<UiAppFixture> { }

public sealed class UiAppFixture : IDisposable
{
    public Application App { get; }
    public UIA2Automation Automation { get; }
    public Window Main { get; }

    private readonly byte[]? _backupSettings;
    private readonly bool _hadSettings;
    private readonly string _workDir;

    public UiAppFixture()
    {
        if (Process.GetProcessesByName("VCamSettingsUi").Length != 0)
            throw new InvalidOperationException(
                "Настройки VCam уже открыты (single-instance мьютекс). " +
                "Закрой окно настроек и повтори прогон.");

        string appData = Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData);
        string dir = Path.Combine(appData, "VCam");
        string settingsPath = Path.Combine(dir, "settings.json");
        _hadSettings = File.Exists(settingsPath);
        _backupSettings = _hadSettings ? File.ReadAllBytes(settingsPath) : null;

        _workDir = Path.Combine(Path.GetTempPath(), "VCamUiTests");
        Directory.CreateDirectory(_workDir);
        string imagePath = Path.Combine(_workDir, "mode_4x3.bmp");
        MakeTestImage(imagePath);

        // Resolve the exe BEFORE writing test settings: UiExePath throws when
        // the UI is not built, and the user's settings.json must not be left
        // overwritten with the test payload (leak seen 08.10.2026).
        string exe = UiExePath(); // бросает FileNotFoundException со списком мест

        Automation = new UIA2Automation();
        try
        {
            WriteTestSettings(settingsPath, imagePath);
            App = Application.Launch(exe);
            var main = App.GetMainWindow(Automation, TimeSpan.FromSeconds(15));
            if (main is null || !main.Title.Contains("VCam"))
                throw new InvalidOperationException("Главное окно настроек не найдено.");
            Main = main;
            BringToFront();
            WaitForPreviewContent(TimeSpan.FromSeconds(15));
        }
        catch
        {
            Automation.Dispose();
            RestoreSettings();
            throw;
        }
    }

    // Путь к свежесобранному UI: output соседнего проекта той же конфигурации.
    // Раскладка зависит от способа сборки: dotnet -> bin/<cfg>/..., MSBuild
    // через solution (Platform=x64) -> bin/x64/<cfg>/... Проверяем обе.
    private static string UiExePath()
    {
        string cfg = Assembly.GetExecutingAssembly()
            .GetCustomAttribute<AssemblyConfigurationAttribute>()?.Configuration ?? "Release";
        string here = AppContext.BaseDirectory; // ...\VCamUiTests\bin\[x64\]<cfg>\net10.0-windows\
        string[] tails = [$"bin/{cfg}/net10.0-windows", $"bin/x64/{cfg}/net10.0-windows"];
        foreach (string tail in tails)
        {
            string exe = Path.GetFullPath(Path.Combine(here, "..", "..", "..", "..",
                "VCamSettingsUi", tail, "VCamSettingsUi.exe"));
            if (File.Exists(exe)) return exe;
        }
        throw new FileNotFoundException(
            "UI не собран: нет VCamSettingsUi.exe ни в bin/<cfg>, ни в bin/x64/<cfg>. " +
            "Собери solution Release|x64 перед прогоном.");
    }

    // Тестовая картинка 800x600 (4:3, асимметричная): fit даёт боковые полосы,
    // cover режет верх/низ — пиксельная разница огромная, ассерт не flaky.
    private static void MakeTestImage(string path)
    {
        using var bmp = new Bitmap(800, 600);
        using var g = Graphics.FromImage(bmp);
        g.Clear(Color.Red);
        g.FillRectangle(Brushes.Blue, 400, 0, 400, 600);
        g.FillRectangle(Brushes.White, 60, 60, 120, 120);
        g.FillRectangle(Brushes.Black, 620, 420, 120, 120);
        bmp.Save(path, System.Drawing.Imaging.ImageFormat.Bmp);
    }

    private static void WriteTestSettings(string settingsPath, string imagePath)
    {
        var payload = new Dictionary<string, object>
        {
            ["source"] = new Dictionary<string, object> { ["type"] = "static" },
            ["static"] = new Dictionary<string, object>
            {
                ["path"] = imagePath,
                ["scaleMode"] = "fit",
                ["cropX"] = 0, ["cropY"] = 0, ["cropW"] = 0, ["cropH"] = 0,
                ["cropKeepAspect"] = false,
            },
            ["video"] = new Dictionary<string, object> { ["path"] = "" },
            ["camera"] = new Dictionary<string, object>
                { ["id"] = "", ["name"] = "", ["capture"] = "max" },
            ["quality"] = "source",
            ["hotkey"] = new Dictionary<string, object> { ["modifiers"] = 3, ["vk"] = 0x56 },
            ["recordHotkey"] = new Dictionary<string, object> { ["modifiers"] = 3, ["vk"] = 0x52 },
            ["record"] = new Dictionary<string, object> { ["path"] = "" },
            ["autostart"] = true,
        };
        Directory.CreateDirectory(Path.GetDirectoryName(settingsPath)!);
        // UTF-8 без BOM — контракт с C++ читателем.
        File.WriteAllText(settingsPath,
            JsonSerializer.Serialize(payload, new JsonSerializerOptions { WriteIndented = true }),
            new UTF8Encoding(false));
    }

    [DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hWnd, int nCmdShow);

    public void BringToFront()
    {
        var h = Main.Properties.NativeWindowHandle.ValueOrDefault;
        if (h != IntPtr.Zero)
        {
            ShowWindow(h, 9 /* SW_RESTORE */);
            SetForegroundWindow(h);
        }
        Main.Focus();
    }

    // Ждём, пока превью покажет картинку, а не чёрный прямоугольник
    // (SetSource отработал после старта формы).
    private void WaitForPreviewContent(TimeSpan timeout)
    {
        var sw = Stopwatch.StartNew();
        while (sw.Elapsed < timeout)
        {
            var el = FindById("preview");
            if (el is not null && !el.IsOffscreen)
            {
                using var shot = Screenshots.Capture(el.BoundingRectangle);
                if (!Screenshots.IsSolidBlack(shot)) return;
            }
            Thread.Sleep(250);
        }
        throw new TimeoutException("Превью не показало тестовую картинку за 15 с.");
    }

    public AutomationElement? FindById(string automationId) =>
        Main.FindFirstDescendant(cf => cf.ByAutomationId(automationId));

    public AutomationElement? FindByName(string name) =>
        Main.FindFirstDescendant(cf => cf.ByName(name));

    // Переключение комбо fit(0)/cover(1)/crop(2) с клавиатуры: WinForms под
    // UIA2 не отдаёт пункты раскрытого ComboBox в дерево (ни Items, ни
    // ListItem), поэтому ведём фокус + HOME/DOWN и сверяем текст значения.
    public void SelectMode(int index)
    {
        string[] tokens = ["fit", "cover", "crop"];
        if (index < 0 || index >= tokens.Length)
            throw new ArgumentOutOfRangeException(nameof(index));
        var combo = FindById("modeCombo")?.AsComboBox()
            ?? throw new InvalidOperationException("modeCombo не найден.");
        BringToFront();
        combo.Focus();
        FlaUI.Core.Input.Keyboard.Press(FlaUI.Core.WindowsAPI.VirtualKeyShort.HOME);
        Thread.Sleep(150);
        for (int k = 0; k < index; k++)
        {
            FlaUI.Core.Input.Keyboard.Press(FlaUI.Core.WindowsAPI.VirtualKeyShort.DOWN);
            Thread.Sleep(150); // WinForms глотает слипшиеся нажатия без паузы
        }
        Thread.Sleep(400); // синхронный UpdateLayout + перерисовка
        string value;
        try { value = combo.Value ?? ""; }
        catch { value = ""; }
        if (!value.StartsWith(tokens[index], StringComparison.OrdinalIgnoreCase))
            throw new InvalidOperationException(
                $"modeCombo: ожидали '{tokens[index]}', показывает '{value}'.");
    }

    private void RestoreSettings()
    {
        try
        {
            string settingsPath = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),
                "VCam", "settings.json");
            if (_hadSettings && _backupSettings is not null)
                File.WriteAllBytes(settingsPath, _backupSettings);
            else if (File.Exists(settingsPath))
                File.Delete(settingsPath);
        }
        catch { /* best-effort: оригинал важнее отчёта */ }
    }

    public void Dispose()
    {
        try
        {
            using var proc = Process.GetProcessById(App.ProcessId);
            if (!proc.HasExited)
            {
                App.Close();
                if (!proc.WaitForExit(5000))
                {
                    proc.Kill();
                    proc.WaitForExit(5000);
                }
            }
        }
        catch { /* best-effort */ }
        Automation.Dispose();
        RestoreSettings();
        GC.SuppressFinalize(this);
    }
}

// Скриншот UIA-элемента + сравнение попиксельно.
public static class Screenshots
{
    public static Bitmap Capture(Rectangle rect)
    {
        var bmp = new Bitmap(Math.Max(1, (int)rect.Width), Math.Max(1, (int)rect.Height));
        using var g = Graphics.FromImage(bmp);
        g.CopyFromScreen((int)rect.X, (int)rect.Y, 0, 0, bmp.Size);
        return bmp;
    }

    public static string Hash(Bitmap bmp)
    {
        var data = bmp.LockBits(new Rectangle(0, 0, bmp.Width, bmp.Height),
            System.Drawing.Imaging.ImageLockMode.ReadOnly, bmp.PixelFormat);
        try
        {
            var bytes = new byte[Math.Abs(data.Stride) * data.Height];
            Marshal.Copy(data.Scan0, bytes, 0, bytes.Length);
            return Convert.ToHexString(MD5.HashData(bytes));
        }
        finally
        {
            bmp.UnlockBits(data);
        }
    }

    public static bool IsSolidBlack(Bitmap bmp, int sample = 16)
    {
        for (int y = 0; y < bmp.Height; y += sample)
            for (int x = 0; x < bmp.Width; x += sample)
                if (bmp.GetPixel(x, y).ToArgb() != Color.Black.ToArgb())
                    return false;
        return true;
    }
}
