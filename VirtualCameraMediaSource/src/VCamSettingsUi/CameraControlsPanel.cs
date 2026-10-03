// Панель ручек физической камеры: 16 строк (название, ползунок, значение,
// метка авто/вручную). Неподдерживаемые (supported=false) — серые, disabled.
// Живой синхрон: опрос сервера каждые ~1.5 с; если картинку крутят мимо нас
// (другая программа, кнопки на камере) — ползунки подтягиваются сами.
// Строку, которую пользователь тащит прямо сейчас, опрос не дёргает.
// Метка «захват сейчас»: читает заголовок v2-секции напрямую (read-only,
// Global→Local), без pipe. Показывает реальный размер захвата, а не настройку.

using System.IO.MemoryMappedFiles;

namespace VCamSettingsUi;

public sealed class CameraControlsPanel : UserControl
{
    // Текст для строки состояния формы; null = всё хорошо, очистить.
    public event Action<string?>? StatusMessage;

    // Тестовая/диагностическая поверхность (также пригодится кнопке «обновить»).
    public int RowCount => _rows.Count;
    public int EnabledRowCount => _rows.Count(r => r.Info.Supported);
    public int DisabledRowCount => _rows.Count(r => !r.Info.Supported);
    public void RefreshNow() => RefreshFromServer();
    [System.ComponentModel.Browsable(false)]
    [System.ComponentModel.DesignerSerializationVisibility(
        System.ComponentModel.DesignerSerializationVisibility.Hidden)]
    public string PipeName { get => _client.PipeName; set => _client.PipeName = value; }

    private readonly Panel _scroll = new();
    private readonly Label _empty = new();
    private readonly System.Windows.Forms.Timer _timer = new();
    private readonly CameraPipeClient _client = new();
    private readonly List<Row> _rows = new();
    private bool _busy; // опрос уже идёт — тик пропускаем (без очереди)
    private readonly Label _nowLine = new(); // «Захват сейчас: WxH»

    // Шрифты презентабельных подсказок (OwnerDraw): заголовок + текст.
    private readonly Font _tipTitleFont = new("Segoe UI", 9f, FontStyle.Bold);
    private readonly Font _tipFont = new("Segoe UI", 9f);
    private const int TipMaxTextWidth = 300;

    protected override void Dispose(bool disposing)
    {
        if (disposing)
        {
            _timer.Stop();
            _timer.Dispose();
            _tipTitleFont.Dispose();
            _tipFont.Dispose();
            foreach (var r in _rows) r.Tip.Dispose();
        }
        base.Dispose(disposing);
    }

    private sealed class Row
    {
        public CameraControlInfo Info = new();
        public Panel Host = new();
        public Label Name = new();
        public TrackBar Slider = new();
        public NumericUpDown Value = new();
        public Label Mode = new();
        public Button Reset = new();
        public bool Editing; // пользователь тащит/печатает прямо сейчас
        public ToolTip Tip = new();
    }

    public CameraControlsPanel()
    {        Name = "cameraControlsPanel";

        _scroll.Dock = DockStyle.Fill;
        _scroll.AutoScroll = true;
        _scroll.BackColor = Color.White;
        _scroll.BorderStyle = BorderStyle.FixedSingle;

        _empty.Dock = DockStyle.Fill;
        _empty.TextAlign = ContentAlignment.MiddleCenter;
        _empty.ForeColor = Color.DimGray;
        _empty.Text = "Переключите эфир на камеру —\r\nздесь появятся ручки настройки.\r\n\r\n" +
            "Сейчас сервер управления недоступен:\r\nхост не открыл камеру.";
        _empty.Visible = false;

        Controls.Add(_scroll);
        Controls.Add(_empty);

        _nowLine.Dock = DockStyle.Top;
        _nowLine.Height = 22;
        _nowLine.TextAlign = ContentAlignment.MiddleLeft;
        _nowLine.ForeColor = Color.DimGray;
        _nowLine.BackColor = Color.White;
        _nowLine.Text = "Захват сейчас: —";
        Controls.Add(_nowLine);

        _timer.Interval = 1500;
        _timer.Tick += (_, _) => RefreshFromServer();
        VisibleChanged += (_, _) =>
        {
            if (Visible) { RefreshFromServer(); _timer.Start(); }
            else _timer.Stop();
        };
    }

    // Первая загрузка/перестроение строк из свежего list.
    private void EnsureRows(List<CameraControlInfo> infos)
    {
        if (SameShape(infos)) return;
        _scroll.SuspendLayout();
        try
        {
            _scroll.Controls.Clear();
            foreach (var r in _rows) r.Tip.Dispose();
            _rows.Clear();
            int y = 4;
            foreach (var info in infos)
            {
                var row = BuildRow(info, y);
                y += 34;
                _rows.Add(row);
                _scroll.Controls.Add(row.Host);
            }
        }
        finally
        {
            _scroll.ResumeLayout();
        }
    }

    private bool SameShape(List<CameraControlInfo> infos)
    {
        if (_rows.Count != infos.Count) return false;
        for (var i = 0; i < infos.Count; i++)
        {
            var a = _rows[i].Info;
            var b = infos[i];
            if (a.Domain != b.Domain || a.Name != b.Name ||
                a.Min != b.Min || a.Max != b.Max || a.Step != b.Step) return false;
        }
        return true;
    }

    private Row BuildRow(CameraControlInfo info, int y)
    {
        var row = new Row { Info = info };
        row.Host.Location = new Point(4, y);
        row.Host.Size = new Size(524, 30);
        row.Host.BackColor = Color.White;

        row.Name.Location = new Point(0, 5);
        row.Name.Size = new Size(132, 20);
        row.Name.Text = CameraControlTexts.TitleOf(info);
        row.Name.AutoEllipsis = true;

        row.Slider.Location = new Point(136, 1);
        row.Slider.Size = new Size(208, 28);
        row.Slider.TickStyle = TickStyle.None;
        SetTrackRange(row.Slider, (int)info.Min, (int)info.Max);
        row.Slider.SmallChange = Math.Max(1, (int)info.Step);
        row.Slider.LargeChange = Math.Max(1, (int)info.Step * 10);
        row.Slider.MouseDown += (_, _) => row.Editing = true;
        row.Slider.MouseUp += (_, _) => { row.Editing = false; ApplyRow(row, 2); };
        row.Slider.KeyUp += (_, _) => { if (!row.Editing) ApplyRow(row, 2); };
        // Сброс к дефолту — мини-кнопкой (даблклик убран как неочевидный).

        row.Value.Location = new Point(348, 3);
        row.Value.Size = new Size(72, 24);
        row.Value.Minimum = (decimal)info.Min;
        row.Value.Maximum = (decimal)info.Max;
        row.Value.Increment = Math.Max(1, (decimal)info.Step);
        row.Value.Enter += (_, _) => row.Editing = true;
        row.Value.Leave += (_, _) => { row.Editing = false; ApplyRow(row, 2); };
        row.Value.KeyDown += (_, e) =>
        {
            if (e.KeyCode == Keys.Enter) { row.Editing = false; ApplyRow(row, 2); e.Handled = true; }
        };

        row.Mode.Location = new Point(424, 5);
        row.Mode.Size = new Size(68, 20);
        row.Mode.ForeColor = Color.DimGray;

        row.Reset.Location = new Point(496, 3);
        row.Reset.Size = new Size(24, 24);
        row.Reset.Text = "↺";
        row.Reset.Click += (_, _) =>
        {
            if (!row.Info.Supported) return;
            // Сброс возвращает заводское состояние: значение по умолчанию +
            // автоматический режим там, где камера его умеет (баланс белого,
            // выдержка). Принудительный manual убивал авто-подстройку света —
            // картинка портилась.
            long flags = (row.Info.Caps & 1) != 0 ? 1 : 2;
            SyncRowWidgets(row, row.Info.Def, flags);
            ApplyRow(row, flags);
        };

        var tip = CameraControlTexts.ToolTipOf(info);
        var resetTip = ResetTipText(info);
        row.Tip.SetToolTip(row.Name, tip);
        row.Tip.SetToolTip(row.Slider, tip);
        row.Tip.SetToolTip(row.Value, tip);
        row.Tip.SetToolTip(row.Reset, resetTip);
        // Презентабельный вид подсказок: свой рисунок вместо системного.
        row.Tip.OwnerDraw = true;
        row.Tip.Popup += (_, e) => TipPopup(e, row);
        row.Tip.Draw += (_, e) => TipDraw(e, row);

        row.Host.Controls.AddRange(new Control[] { row.Name, row.Slider, row.Value, row.Mode, row.Reset });
        SyncRowWidgets(row, info.Cur, info.Flags);
        ApplySupportedLook(row);
        return row;
    }

    // TrackBar бросает исключение, если Minimum/Maximum ставить не в том
    // порядке (например, Minimum=2800 при Maximum=10). Порядок выбираем так,
    // чтобы промежуточное состояние всегда было валидным.
    private static void SetTrackRange(TrackBar t, int min, int max)
    {
        if (max <= min) max = min + 1; // защита от битого диапазона драйвера
        if (min >= t.Maximum) { t.Maximum = max; t.Minimum = min; }
        else { t.Minimum = min; t.Maximum = max; }
        t.Value = Math.Clamp(t.Value, min, max);
    }

    private static string ResetTipText(CameraControlInfo info) =>
        (info.Caps & 1) != 0
            ? $"Сбросить к значению по умолчанию ({info.Def}) и вернуть автоматический режим."
            : $"Сбросить к значению по умолчанию ({info.Def}).";

    private void TipPopup(PopupEventArgs e, Row row)
    {
        var title = CameraControlTexts.TitleOf(row.Info);
        var body = e.AssociatedControl == row.Reset
            ? ResetTipText(row.Info)
            : CameraControlTexts.ToolTipOf(row.Info);
        var flags = TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl;
        var tSize = TextRenderer.MeasureText(title, _tipTitleFont,
            new Size(TipMaxTextWidth, 0), flags);
        var bSize = TextRenderer.MeasureText(body, _tipFont,
            new Size(TipMaxTextWidth, 0), flags);
        int textW = Math.Max(tSize.Width, bSize.Width);
        e.ToolTipSize = new Size(textW + 34 + 20, tSize.Height + 4 + bSize.Height + 20);
    }

    private void TipDraw(DrawToolTipEventArgs e, Row row)
    {
        var title = CameraControlTexts.TitleOf(row.Info);
        var body = e.AssociatedControl == row.Reset
            ? ResetTipText(row.Info)
            : CameraControlTexts.ToolTipOf(row.Info);
        e.Graphics.FillRectangle(SystemBrushes.Info, e.Bounds);
        e.Graphics.DrawIcon(SystemIcons.Information, new Rectangle(10, 10, 16, 16));
        const int x = 34;
        var flags = TextFormatFlags.WordBreak | TextFormatFlags.TextBoxControl;
        var tSize = TextRenderer.MeasureText(title, _tipTitleFont,
            new Size(TipMaxTextWidth, 0), flags);
        TextRenderer.DrawText(e.Graphics, title, _tipTitleFont,
            new Point(x, 8), SystemColors.InfoText, flags);
        TextRenderer.DrawText(e.Graphics, body, _tipFont,
            new Rectangle(x, 8 + tSize.Height + 4, e.Bounds.Width - x - 10,
                e.Bounds.Height), SystemColors.InfoText, flags);
        e.DrawBorder();
    }

    private static void ApplySupportedLook(Row row)
    {
        bool on = row.Info.Supported;
        row.Name.Enabled = on;
        row.Slider.Enabled = on;
        row.Value.Enabled = on;
        row.Reset.Enabled = on;
        row.Mode.Text = CameraControlTexts.ModeMark(row.Info);
    }

    // Выставить виджеты строки (вызывается и из опроса, и из apply).
    private static void SyncRowWidgets(Row row, long cur, long flags)
    {
        var info = row.Info;
        info.Cur = cur;
        info.Flags = flags;
        long clamped = Math.Clamp(cur, info.Min, info.Max);
        // Диапазоны заданы при построении строки и не меняются (SameShape).
        if (row.Slider.Value != (int)clamped) row.Slider.Value = (int)clamped;
        if (!row.Value.Focused && row.Value.Value != (decimal)clamped)
            row.Value.Value = (decimal)clamped;
        row.Mode.Text = CameraControlTexts.ModeMark(info);
    }

    // Реальный размер захвата из заголовка v2 (read-only). (0,0) = нет данных.
    private static (uint w, uint h) ReadV2Dims()
    {
        foreach (var name in new[] { "Global\\VCam.FrameBuffer.v2", "Local\\VCam.FrameBuffer.v2" })
        {
            try
            {
                using var mmf = MemoryMappedFile.OpenExisting(name, MemoryMappedFileRights.Read);
                using var acc = mmf.CreateViewAccessor(0, 16, MemoryMappedFileAccess.Read);
                if (acc.ReadUInt32(0) != 0x5643414D || acc.ReadUInt32(4) != 2) continue;
                uint w = acc.ReadUInt32(8), h = acc.ReadUInt32(12);
                if (w > 0 && h > 0 && w <= 3840 && h <= 2160) return (w, h);
            }
            catch { /* нет секции/доступа — пробуем дальше */ }
        }
        return (0, 0);
    }

    private void RefreshFromServer()
    {
        if (_busy || !Visible) return;
        _busy = true;
        try
        {
            if (!_client.TryList(out var infos, out _))
            {
                ShowEmpty();
                return;
            }
            if (infos.Count == 0)
            {
                ShowEmpty();
                return;
            }
            EnsureRows(infos);
            _scroll.Visible = true;
            _empty.Visible = false;
            for (var i = 0; i < infos.Count; i++)
            {
                var row = _rows[i];
                var snap = infos[i];
                row.Info.Supported = snap.Supported;
                row.Info.Caps = snap.Caps;
                row.Info.Def = snap.Def;
                ApplySupportedLook(row);
                if (!row.Info.Supported) continue;
                if (row.Editing) continue; // не дёргать, пока юзер тащит
                if (row.Value.Focused) continue; // не перебивать ручной ввод
                SyncRowWidgets(row, snap.Cur, snap.Flags);
            }
            var (vw, vh) = ReadV2Dims();
            _nowLine.Text = (vw > 0) ? $"Захват сейчас: {vw}×{vh}" : "Захват сейчас: —";
            StatusMessage?.Invoke(null);
        }
        finally
        {
            _busy = false;
        }
    }

    private void ShowEmpty()
    {
        _scroll.Visible = false;
        _empty.Visible = true;
        var (vw, vh) = ReadV2Dims();
        _nowLine.Text = (vw > 0) ? $"Захват сейчас: {vw}×{vh}" : "Захват сейчас: —";
    }

    private void ApplyRow(Row row, long flags)
    {
        if (!row.Info.Supported || !Visible) return;
        long want = (long)row.Value.Value;
        if (row.Slider.Enabled) want = row.Slider.Value;
        if (_client.TrySet(row.Info.Domain, row.Info.Name, want, flags,
                out var applied, out var appliedFlags, out var error))
        {
            SyncRowWidgets(row, applied, appliedFlags);
            StatusMessage?.Invoke(null);
        }
        else
        {
            // Тихо в хинт, без падений; следующий опрос вернёт правду.
            StatusMessage?.Invoke($"«{CameraControlTexts.TitleOf(row.Info)}» не применилось: {error}.");
        }
    }
}
