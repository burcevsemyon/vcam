// Модальный захват комбинации для глобального хоткея (RegisterHotKey хоста).
// Валидация зеркалит Settings.ParseHotkey*: модификаторы 1-15 (хотя бы один
// из Ctrl/Alt/Shift/Win), vk 0x08-0xFE. Биты — RegisterHotKey:
// Alt=1, Ctrl=2, Shift=4, Win=8. Голая клавиша без модификатора запрещена —
// глобальный перехват без модификаторов ломает ввод во всех приложениях.

using System.Runtime.InteropServices;
using System.Text;

namespace VCamSettingsUi;

public sealed class HotkeyEditForm : Form
{
    [DllImport("user32.dll")]
    private static extern short GetAsyncKeyState(int vKey);

    private const int VkLWin = 0x5B;
    private const int VkRWin = 0x5C;

    private readonly Label _combo;
    private readonly Label _warning;

    public int Modifiers { get; private set; }
    public int Vk { get; private set; }

    public HotkeyEditForm(string title, int modifiers, int vk)
    {
        Modifiers = modifiers;
        Vk = vk;

        Text = title;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(440, 180);
        Font = new Font("Segoe UI", 9f);
        Name = "hotkeyEditForm";
        KeyPreview = true;

        var hint = new Label
        {
            Location = new Point(16, 14),
            Size = new Size(408, 22),
            Text = "Нажмите новую комбинацию клавиш…",
            Name = "hotkeyEditHint",
        };

        _combo = new Label
        {
            Location = new Point(16, 44),
            Size = new Size(408, 40),
            Font = new Font(Font.FontFamily, 16f, FontStyle.Bold),
            Name = "hotkeyEditCombo",
        };

        _warning = new Label
        {
            Location = new Point(16, 92),
            Size = new Size(408, 36),
            ForeColor = Color.DimGray,
            Name = "hotkeyEditWarning",
        };

        var ok = new Button
        {
            Location = new Point(228, 138),
            Size = new Size(96, 30),
            Text = "OK",
            DialogResult = DialogResult.OK,
            Name = "hotkeyEditOk",
        };
        var cancel = new Button
        {
            Location = new Point(332, 138),
            Size = new Size(92, 30),
            Text = "Отмена",
            DialogResult = DialogResult.Cancel,
            Name = "hotkeyEditCancel",
        };

        AcceptButton = ok;
        CancelButton = cancel;
        Controls.AddRange(new Control[] { hint, _combo, _warning, ok, cancel });

        UpdateDisplay();
        KeyDown += OnComboKeyDown;
    }

    private void OnComboKeyDown(object? sender, KeyEventArgs e)
    {
        var key = e.KeyCode;
        if (key is Keys.None or Keys.ControlKey or Keys.ShiftKey or Keys.Menu
            or Keys.LWin or Keys.RWin or Keys.Packet)
        {
            _warning.Text = "Это модификатор — добавьте основную клавишу (букву, F-клавишу и т.п.).";
            return;
        }

        int mods = 0;
        if (e.Control) mods |= 2;
        if (e.Alt) mods |= 1;
        if (e.Shift) mods |= 4;
        if ((GetAsyncKeyState(VkLWin) & 0x8000) != 0 || (GetAsyncKeyState(VkRWin) & 0x8000) != 0)
            mods |= 8;

        if (mods == 0)
        {
            _warning.Text = "Нужен хотя бы один модификатор: Ctrl, Alt, Shift или Win (иначе комбинация перехватит обычный ввод).";
            return;
        }

        int vk = (int)key;
        if (vk < 0x08 || vk > 0xFE)
        {
            _warning.Text = $"Клавиша VK 0x{vk:X2} вне диапазона 0x08-0xFE — выберите другую.";
            return;
        }

        Modifiers = mods;
        Vk = vk;
        e.Handled = true;
        e.SuppressKeyPress = true;
        _warning.Text = "Хост применит её сразу после сохранения настроек (~1 с).";
        UpdateDisplay();
    }

    private void UpdateDisplay()
    {
        _combo.Text = Display(Modifiers, Vk);
    }

    // Зеркало MainForm.HotkeyDisplayMods / хоста HotkeyDisplay (C++).
    private static string Display(int mods, int vk)
    {
        var sb = new StringBuilder();
        if ((mods & 2) != 0) sb.Append("Ctrl+");
        if ((mods & 1) != 0) sb.Append("Alt+");
        if ((mods & 4) != 0) sb.Append("Shift+");
        if ((mods & 8) != 0) sb.Append("Win+");
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
            sb.Append((char)vk);
        else if (vk >= 0x70 && vk <= 0x87)
            sb.Append('F').Append(vk - 0x70 + 1);
        else
            sb.Append(vk switch
            {
                0x20 => "Space",
                0x0D => "Enter",
                0x09 => "Tab",
                0x1B => "Esc",
                0x25 => "Left",
                0x27 => "Right",
                0x26 => "Up",
                0x28 => "Down",
                _ => $"VK 0x{vk:X2}",
            });
        return sb.ToString();
    }
}
