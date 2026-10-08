using System.Diagnostics;
using System.Text;
using System.Text.Json;

namespace VCamSettingsUi;

public sealed class MainForm : Form
{
    private readonly PictureBox _preview = new();
    private readonly CropPreviewControl _cropView = new();
    private readonly ComboBox _mode = new();
    private readonly Button _openButton = new();
    private readonly Button _fullSizeButton = new();
    private readonly Button _saveButton = new();
    private readonly Button _reloadButton = new();
    private readonly Label _pathLabel = new();
    private readonly Label _hintLabel = new();

    // Media switch: still image (StaticProducer) or video clip (VideoProducer).
    private readonly Label _mediaLabel = new();
    private readonly ComboBox _mediaCombo = new();

    // v2 frame quality (phase vcam-quality-v2): native source size or fixed 720p.
    private readonly Label _qualityLabel = new();
    private readonly ComboBox _qualityCombo = new();

    // Info panel replacing the picture preview in video mode.
    private readonly Panel _videoPanel = new();
    private readonly Label _videoTitle = new();
    private readonly Label _videoPathLabel = new();
    private readonly Label _videoInfoLabel = new();
    private readonly Button _previewButton = new();
    private readonly Label _previewHint = new();
    // Borrowed-video indicator (hotkey static→video→auto-static): visible only
    // in video mode, reads the transient %APPDATA%\VCam\hotkey_state.json the
    // host writes while a hotkey-borrowed clip is on air.
    private readonly Label _hotkeyBorrowLabel = new();
    // Clip options: video.loop checkbox + videoHotkey (play/pause) hint line
    // with the «Изменить…» editor (same pattern as the record hotkey row).
    private readonly CheckBox _videoLoopCheck = new();
    private readonly Label _videoHotkeyHint = new();
    private readonly Button _videoHotkeyEdit = new();
    private int _videoHotkeyMods = 3, _videoHotkeyVk = 0x50;

    // Physical camera panel (replaces the preview in camera mode). The device
    // list is produced by "VCamProducerCli list-devices" (stdout rows id\tname).
    private readonly Panel _cameraPanel = new();
    private readonly Label _cameraTitle = new();
    private readonly Label _cameraDevLabel = new();
    private readonly ComboBox _cameraCombo = new();
    private readonly Button _cameraRefresh = new();
    private readonly Label _cameraIdLabel = new();
    private readonly TextBox _cameraHint = new();
    private readonly Label _cameraStatus = new();
    // Capture height selector (camera.capture): max (default) | 720p | 1080p.
    private readonly Label _captureLabel = new();
    private readonly ComboBox _captureCombo = new();
    private readonly CameraControlsPanel _controlsPanel = new();
    private string _cameraListStatus = "";

    // One ComboBox row: a device (or the remembered device that is missing now).
    private sealed class CameraItem(string id, string name, bool missing)
    {
        public string Id { get; } = id;
        public string Name { get; } = name;
        public bool Missing { get; } = missing;
        public override string ToString() => Missing ? $"{Name} (недоступно)" : Name;
    }

    private readonly List<CameraItem> _cameraItems = new();
    private bool _cameraListLoaded;
    private string _cameraWishId = "";   // remembered choice from settings.json
    private string _cameraWishName = "";
    private string? _cliExe;

    // Crop rect fields (source pixels), visible only in crop mode.
    private readonly NumericUpDown _cropX = new();
    private readonly NumericUpDown _cropY = new();
    private readonly NumericUpDown _cropW = new();
    private readonly NumericUpDown _cropH = new();
    private readonly Label _cropXLabel = new();
    private readonly Label _cropYLabel = new();
    private readonly Label _cropWLabel = new();
    private readonly Label _cropHLabel = new();
    private readonly CheckBox _cropKeepAspect = new();

    private Image? _sourceImage;
    private string _sourcePath = "";
    private string _videoPath = "";
    private string? _previewExe;
    private bool _updatingCropFields;

    // Hotkey hint (always visible, single line between the mode row and the
    // crop fields): current combination from settings.json + borrow state.
    // The combination is edited via the «Изменить…» button (HotkeyEditForm,
    // global-key capture); values land in settings.json on «Сохранить» and the
    // host re-registers the hotkey live (~1 с). Invalid file values fall back
    // to Ctrl+Alt+V in host+UI.
    private readonly Label _hotkeyHint = new();
    private readonly Button _hotkeyEdit = new();
    private readonly Button _recHotkeyEdit = new();
    private int _hotkeyMods = 3, _hotkeyVk = 0x56;
    private int _recHotkeyMods = 3, _recHotkeyVk = 0x52;

    // Ether recording to .mp4 (host SinkWriter, frames as broadcast):
    // path box (default in settings "record"), browse, start/stop button,
    // REC indicator. The on/off STATE is transient
    // (%APPDATA%\VCam\record_state.json, written by the host, NOT settings);
    // start/stop commands go via record_command.json in the same directory.
    private readonly GroupBox _recGroup = new();
    private readonly Label _recPathLabel = new();
    private readonly TextBox _recPathText = new();
    private readonly Button _recBrowse = new();
    private readonly Button _recButton = new();
    private readonly Label _recStatus = new();
    private readonly Label _recHint = new();
    private bool _recRecording;
    private string _lastRecPath = "";

    // Host (VCamVideoStreamProducer.exe): start/stop button + status indicator.
    private readonly Button _hostButton = new();
    private readonly Label _hostStatusLabel = new();
    private readonly System.Windows.Forms.Timer _hostTimer = new();
    private readonly Button _helpButton = new();
    private string? _hostExe;

    private const string HostMutexName = "VCamVideoStreamProducer.Instance";
    private const string HostStopEventName = "VCamVideoStreamProducer.Stop";

    // Live sync with settings.json (the file can be rewritten from outside —
    // e2e, tooling — while the form is open). A FileSystemWatcher + 250 ms
    // debounce timer reloads the controls; user edits set _dirty and block
    // the auto-reload (a warning in the hint label + the manual "Обновить"
    // button appear instead). Echo of our own Save is recognised by content
    // comparison (_lastAppliedText), not by timing.
    private FileSystemWatcher? _settingsWatcher;
    private readonly System.Windows.Forms.Timer _syncTimer = new();
    private bool _dirty;            // user edited controls after Load/Save
    private bool _suppressDirty;    // programmatic control updates (Load/reload)
    // В3: трогал ли пользователь выбор медиа после последнего Apply.
    // Нужен, чтобы Save не схлопывал неизвестный будущий source.type в
    // "static", когда комбо показывает fallback-вид (см. Collect).
    private bool _mediaChangedByUser;
    private string _lastAppliedText = ""; // file snapshot the controls reflect
    private int _syncRetries;

    private static readonly string[] ModeNames = { "fit — вписать с пололосами", "cover — заполнить (обрезка)", "crop — обрезка выбранной области" };
    private static readonly string[] MediaNames = { "статичная картинка", "видеоролик", "физическая камера" };
    private static readonly string[] QualityNames = { "натив (source)", "720p (fixed)", "1080p (fixed)" };
    private static readonly string[] CaptureNames = { "Максимум", "720p", "1080p" };

    public MainForm()
    {
        Text = "VCam — настройки трансляции";
        FormBorderStyle = FormBorderStyle.Sizable;
        MaximizeBox = true;
        MinimizeBox = true;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(880, 784);
        MinimumSize = new Size(900, 834);
        Font = new Font("Segoe UI", 9f);
        try
        {
            // Иконка окна = иконка exe (кладётся ApplicationIcon в csproj).
            var exeIcon = Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            if (exeIcon is not null) Icon = exeIcon;
        }
        catch (Exception ex)
        {
            // Без иконки окно тоже работает; молча не глотаем.
            Debug.WriteLine($"MainForm: no exe icon: {ex.Message}");
        }

        SetupPreview();
        SetupVideoPanel();
        SetupCameraPanel();
        SetupSourceRow();
        SetupRecGroup();
        SetupHostRow();
        SetupCropFields();
        SetupFooter();

        Controls.AddRange(new Control[] { _preview, _cropView, _videoPanel, _cameraPanel, _pathLabel, _mediaLabel, _mediaCombo,
            _qualityLabel, _qualityCombo, _recGroup, _hotkeyHint, _hotkeyEdit,
            _mode, _openButton, _fullSizeButton, _saveButton, _reloadButton, _hostStatusLabel, _hostButton, _helpButton,
            _cropXLabel, _cropX, _cropYLabel, _cropY, _cropWLabel, _cropW, _cropHLabel, _cropH, _cropKeepAspect, _hintLabel });

        SetupTabOrder();
        SetupHostTimer();
        WireEvents();

        SubscribeDirtyTracking();
        LoadCurrentSettings();
        UpdateLayout();
        InitSettingsSync();
    }
    private void SetupPreview()
    {
        _preview.Location = new Point(12, 12);
        _preview.Size = new Size(856, 455); // preview box; Zoom letterboxes 16:9 on black
        _preview.SizeMode = PictureBoxSizeMode.Zoom;
        _preview.BackColor = Color.Black;
        _preview.BorderStyle = BorderStyle.FixedSingle;
        _preview.Name = "preview";
        _preview.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        _cropView.Location = _preview.Location;
        _cropView.Size = _preview.Size;
        _cropView.Visible = false;
        _cropView.SelectionChanged += OnCropSelectionChanged;
    }

    private void SetupSourceRow()
    {
        _pathLabel.Location = new Point(12, 472);
        _pathLabel.Size = new Size(856, 20);
        _pathLabel.Text = "(файл не выбран)";
        _pathLabel.AutoEllipsis = true;
        _pathLabel.Name = "pathLabel";

        _mediaLabel.Location = new Point(12, 502);
        _mediaLabel.Size = new Size(60, 18);
        _mediaLabel.Text = "Медиа:";
        _mediaLabel.Name = "mediaLabel";

        _mediaCombo.Location = new Point(76, 498);
        _mediaCombo.Size = new Size(236, 28);
        _mediaCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _mediaCombo.Items.AddRange(MediaNames);
        _mediaCombo.SelectedIndex = 0;
        _mediaCombo.Name = "mediaCombo";

        // Same row as the media switch (gap between open/save buttons: x=490..680).
        _qualityLabel.Location = new Point(494, 502);
        _qualityLabel.Size = new Size(64, 18);
        _qualityLabel.Text = "Качество:";
        _qualityLabel.Name = "qualityLabel";

        _qualityCombo.Location = new Point(562, 498);
        _qualityCombo.Size = new Size(112, 28);
        _qualityCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _qualityCombo.Items.AddRange(QualityNames);
        _qualityCombo.SelectedIndex = 0;
        _qualityCombo.Name = "qualityCombo";

        _mode.Location = new Point(12, 532);
        _mode.Size = new Size(250, 28);
        _mode.DropDownStyle = ComboBoxStyle.DropDownList;
        _mode.Items.AddRange(ModeNames);
        _mode.SelectedIndex = 0;
        _mode.Name = "modeCombo";

        _openButton.Location = new Point(320, 498);
        _openButton.Size = new Size(170, 30);
        _openButton.Text = "Открыть картинку…";
        _openButton.Name = "openButton";
        _openButton.Click += OnOpenClicked;

        _fullSizeButton.Location = new Point(268, 532);
        _fullSizeButton.Size = new Size(150, 30);
        _fullSizeButton.Text = "Просмотр полный";
        _fullSizeButton.Enabled = false;
        _fullSizeButton.Name = "fullSizeButton";
        _fullSizeButton.Click += OnFullSizeClicked;

        _saveButton.Location = new Point(680, 498);
        _saveButton.Size = new Size(188, 30);
        _saveButton.Text = "Сохранить настройки";
        _saveButton.Name = "saveButton";
        _saveButton.Click += OnSaveClicked;
    }

    private void SetupHostRow()
    {
        // Manual reload from settings.json (always available; also the way out
        // when the file changed externally while the form is dirty).
        _reloadButton.Location = new Point(566, 632);
        _reloadButton.Size = new Size(116, 40);
        _reloadButton.Text = "Обновить";
        _reloadButton.Name = "reloadButton";
        _reloadButton.Click += OnReloadClicked;
        _reloadButton.Anchor = AnchorStyles.Bottom;

        _hostStatusLabel.Location = new Point(430, 538);
        _hostStatusLabel.Size = new Size(244, 22);
        _hostStatusLabel.ForeColor = Color.DimGray;
        _hostStatusLabel.Name = "hostStatusLabel";

        _hostButton.Location = new Point(680, 532);
        _hostButton.Size = new Size(188, 30);
        _hostButton.Name = "hostButton";
        _hostButton.Click += OnHostButtonClicked;

        // Hotkey hint: single always-visible line under the mode row.
        _hotkeyHint.Location = new Point(12, 562);
        _hotkeyHint.Size = new Size(740, 22);
        _hotkeyHint.ForeColor = Color.DimGray;
        _hotkeyHint.Name = "hotkeyHint";
        _hotkeyHint.Anchor = AnchorStyles.Top | AnchorStyles.Left;

        _hotkeyEdit.Location = new Point(758, 558);
        _hotkeyEdit.Size = new Size(110, 28);
        _hotkeyEdit.Text = "Изменить…";
        _hotkeyEdit.Name = "hotkeyEditButton";
        _hotkeyEdit.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        _hotkeyEdit.Click += OnHotkeyEditClicked;
    }

    private void SetupCropFields()
    {
        int fieldY = 588;
        PlaceCropField(_cropXLabel, _cropX, 12, fieldY, "X");
        PlaceCropField(_cropYLabel, _cropY, 140, fieldY, "Y");
        PlaceCropField(_cropWLabel, _cropW, 268, fieldY, "Ширина");
        PlaceCropField(_cropHLabel, _cropH, 430, fieldY, "Высота");
        foreach (var n in new[] { _cropX, _cropY, _cropW, _cropH })
            n.ValueChanged += OnCropFieldChanged;

        _cropKeepAspect.Location = new Point(580, fieldY + 2);
        _cropKeepAspect.Size = new Size(200, 22);
        _cropKeepAspect.Text = "Сохранять пропорции";
        _cropKeepAspect.Visible = false;
    }

    private void SetupFooter()
    {
        _hintLabel.Location = new Point(12, 634);
        _hintLabel.Size = new Size(548, 38);
        _hintLabel.ForeColor = Color.DimGray;
        _hintLabel.Text = $"Настройки: {Settings.FilePath} — хост VCam подхватит их автоматически (~1 с).";
        _hintLabel.Name = "hintLabel";
        _hintLabel.Anchor = AnchorStyles.Left | AnchorStyles.Bottom;

        _helpButton.Location = new Point(688, 632);
        _helpButton.Size = new Size(180, 40);
        _helpButton.Text = "Справка…";
        _helpButton.Name = "helpButton";
        _helpButton.Click += OnHelpClicked;
        _helpButton.Anchor = AnchorStyles.Right | AnchorStyles.Bottom;
    }

    private void SetupTabOrder()
    {
        // М1: таб-порядок = визуальному (сверху вниз). AddRange выше идёт не
        // по визуали — выставляем TabIndex явно (только TabStop-контролы
        // реально участвуют в Tab, остальным индекс безвреден).
        var tabVisual = new Control[]
        {
            _preview, _cropView, _videoPanel, _cameraPanel,
            _mediaCombo, _openButton, _qualityCombo, _saveButton,
            _mode, _fullSizeButton, _hostButton,
            _cropX, _cropY, _cropW, _cropH, _cropKeepAspect,
            _recGroup, _reloadButton, _helpButton,
        };
        for (var ti = 0; ti < tabVisual.Length; ti++) tabVisual[ti].TabIndex = ti;
        _videoPanel.TabIndex = 2;
        _cameraPanel.TabIndex = 3;
        _previewButton.TabIndex = 0;
        _cameraCombo.TabIndex = 0;
        _cameraRefresh.TabIndex = 1;
        _captureCombo.TabIndex = 2;
        _recPathText.TabIndex = 0;
        _recBrowse.TabIndex = 1;
        _recButton.TabIndex = 2;
    }

    private void SetupHostTimer()
    {
        _previewExe = FindPreviewExe();
        _hostExe = FindHostExe();
        _hostTimer.Interval = 1000;
        _hostTimer.Tick += (_, _) => UpdateHostStatus();
        _hostTimer.Start();
        UpdateHostStatus();
    }

    private void WireEvents()
    {
        _mode.SelectedIndexChanged += (_, _) => UpdateLayout();
        _mediaCombo.SelectedIndexChanged += (_, _) =>
        {
            if (!_suppressDirty) _mediaChangedByUser = true; // В3: явный выбор медиа
            UpdateLayout();
        };
        _cameraCombo.SelectedIndexChanged += (_, _) => UpdateCameraPanel();
        _cameraRefresh.Click += OnCameraRefreshClicked;
        _controlsPanel.StatusMessage += UpdateCameraStatus;
    }


    private void SetupVideoPanel()
    {
        _videoPanel.Location = new Point(12, 12);
        _videoPanel.Size = new Size(856, 455);
        _videoPanel.BorderStyle = BorderStyle.FixedSingle;
        _videoPanel.BackColor = Color.White;
        _videoPanel.Visible = false;
        _videoPanel.Name = "videoPanel";

        _videoTitle.Font = new Font(Font.FontFamily, 11f, FontStyle.Bold);
        _videoTitle.Location = new Point(16, 16);
        _videoTitle.Size = new Size(820, 26);
        _videoTitle.Text = "Видеоролик";

        _videoPathLabel.Location = new Point(16, 50);
        _videoPathLabel.Size = new Size(820, 22);
        _videoPathLabel.AutoEllipsis = true;
        _videoPathLabel.Name = "videoPathLabel";

        _videoInfoLabel.Location = new Point(16, 84);
        _videoInfoLabel.Size = new Size(820, 94);
        _videoInfoLabel.ForeColor = Color.DimGray;
        _videoInfoLabel.Text =
            "Ролик декодируется хостом VCamVideoStreamProducer.exe и всегда масштабируется letterbox в 1280×720.\r\n" +
            "Настройки scaleMode и crop для видео не применяются (см. секцию static).\r\n" +
            "Смена файла подхватывается автоматически (~1 с), без перезапуска.\r\n" +
            "Повтор выключен — ролик стартует с первого кадра на паузе (запуск — play), " +
            "после конца кадр замирает (как в плеере); включён — автозапуск по кругу.\r\n" +
            "Ролик, вызванный горячей клавишей показа видео, играет один раз и " +
            "возвращает предыдущий источник.\r\n" +
            "Встроенное превью картинки — то же, что идёт в эфир.";

        _previewButton.Location = new Point(16, 184);
        _previewButton.Size = new Size(380, 40);
        _previewButton.Text = "Открыть окно предпросмотра (VCamPreview)";
        _previewButton.Name = "previewButton";
        _previewButton.Click += OnPreviewClicked;

        // video.loop: saved on «Сохранить», the host re-creates the source
        // (~1 с) — a running clip restarts with the new loop mode.
        _videoLoopCheck.Location = new Point(410, 192);
        _videoLoopCheck.Size = new Size(280, 24);
        _videoLoopCheck.Text = "Повтор ролика (по кругу)";
        _videoLoopCheck.Name = "videoLoopCheck";
        _videoLoopCheck.CheckedChanged += (_, _) => MarkDirty();

        _previewHint.Location = new Point(16, 228);
        _previewHint.Size = new Size(820, 64);
        _previewHint.ForeColor = Color.DimGray;
        _previewHint.Name = "previewHint";

        // videoHotkey row (play/pause): hint + editor, mirrors the record row.
        _videoHotkeyHint.Location = new Point(16, 298);
        _videoHotkeyHint.Size = new Size(640, 22);
        _videoHotkeyHint.ForeColor = Color.DimGray;
        _videoHotkeyHint.Name = "videoHotkeyHint";
        UpdateVideoHotkeyHint();

        _videoHotkeyEdit.Location = new Point(666, 294);
        _videoHotkeyEdit.Size = new Size(110, 28);
        _videoHotkeyEdit.Text = "Изменить…";
        _videoHotkeyEdit.Name = "videoHotkeyEditButton";
        _videoHotkeyEdit.Click += OnVideoHotkeyEditClicked;

        // Borrowed-video indicator: empty unless the host holds a hotkey borrow
        // (transient hotkey_state.json). Free space below the preview hint.
        _hotkeyBorrowLabel.Location = new Point(16, 330);
        _hotkeyBorrowLabel.Size = new Size(820, 110);
        _hotkeyBorrowLabel.ForeColor = Color.DimGray;
        _hotkeyBorrowLabel.Name = "hotkeyBorrowLabel";

        _videoPanel.Controls.AddRange(new Control[] { _videoTitle, _videoPathLabel, _videoInfoLabel, _previewButton, _videoLoopCheck, _previewHint, _videoHotkeyHint, _videoHotkeyEdit, _hotkeyBorrowLabel });
    }

    // Same style as _videoPanel: white, FixedSingle, 856x455 over the preview.
    private void SetupCameraPanel()
    {
        _cameraPanel.Location = new Point(12, 12);
        _cameraPanel.Size = new Size(856, 455);
        _cameraPanel.BorderStyle = BorderStyle.FixedSingle;
        _cameraPanel.BackColor = Color.White;
        _cameraPanel.Visible = false;
        _cameraPanel.Name = "cameraPanel";

        _cameraTitle.Font = new Font(Font.FontFamily, 11f, FontStyle.Bold);
        _cameraTitle.Location = new Point(16, 16);
        _cameraTitle.Size = new Size(820, 26);
        _cameraTitle.Text = "Физическая камера";

        _cameraDevLabel.Location = new Point(16, 56);
        _cameraDevLabel.Size = new Size(62, 18);
        _cameraDevLabel.Text = "Камера:";
        _cameraDevLabel.Name = "cameraDevLabel";

        _cameraCombo.Location = new Point(80, 52);
        _cameraCombo.Size = new Size(556, 28);
        _cameraCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _cameraCombo.Name = "cameraCombo";

        _cameraRefresh.Location = new Point(648, 51);
        _cameraRefresh.Size = new Size(190, 30);
        _cameraRefresh.Text = "Обновить список";
        _cameraRefresh.Name = "cameraRefresh";

        _captureLabel.Location = new Point(16, 94);
        _captureLabel.Size = new Size(62, 18);
        _captureLabel.Text = "Захват:";
        _captureLabel.Name = "captureLabel";

        _captureCombo.Location = new Point(80, 90);
        _captureCombo.Size = new Size(200, 28);
        _captureCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _captureCombo.Items.AddRange(CaptureNames);
        _captureCombo.SelectedIndex = 0;
        _captureCombo.Name = "captureCombo";

        _cameraIdLabel.Location = new Point(16, 124);
        _cameraIdLabel.Size = new Size(820, 20);
        _cameraIdLabel.AutoEllipsis = true;
        _cameraIdLabel.ForeColor = Color.DimGray;
        _cameraIdLabel.Name = "cameraIdLabel";

        // Live camera knobs (left) + hint/status column (right). The hint keeps
        // its full text — TextBox with scrolling instead of a clipped Label.
        _controlsPanel.Location = new Point(16, 150);
        _controlsPanel.Size = new Size(524, 270);

        _cameraHint.Location = new Point(548, 150);
        _cameraHint.Size = new Size(292, 160);
        _cameraHint.Multiline = true;
        _cameraHint.ReadOnly = true;
        _cameraHint.ScrollBars = ScrollBars.Vertical;
        _cameraHint.BorderStyle = BorderStyle.None;
        _cameraHint.BackColor = Color.White;
        _cameraHint.ForeColor = Color.DimGray;
        _cameraHint.Name = "cameraHint";
        _cameraHint.Text =
            "Выберите камеру — она будет транслироваться в виртуальную камеру VCam.\r\n" +
            "Список формирует VCamProducerCli list-devices; кнопка «Обновить список» перечитывает устройства\r\n" +
            "(можно нажать после подключения новой камеры, перезапуск не нужен).\r\n\r\n" +
            "Если камера отсутствует, отключена или занята другой программой — хост покажет NO SIGNAL\r\n" +
            "и продолжит попытки открыть её (штатные ретраи). Пустая секция camera тоже даёт NO SIGNAL.\r\n\r\n" +
            "Устройство с пометкой «(недоступно)» — сохранённый выбор, которого сейчас нет в системе:\r\n" +
            "его id сохраняется, пока вы не выберете другую камеру.";

        _cameraStatus.Location = new Point(548, 316);
        _cameraStatus.Size = new Size(292, 104);
        _cameraStatus.ForeColor = Color.DimGray;
        _cameraStatus.Name = "cameraStatus";

        _cameraPanel.Controls.AddRange(new Control[]
            { _cameraTitle, _cameraDevLabel, _cameraCombo, _cameraRefresh, _captureLabel, _captureCombo,
              _cameraIdLabel, _controlsPanel, _cameraHint, _cameraStatus });
    }

    // Ether recording group: file path + browse + start/stop + REC line +
    // record-hotkey hint. Below the crop fields.
    private void SetupRecGroup()
    {
        _recGroup.Location = new Point(12, 614);
        _recGroup.Size = new Size(856, 110);
        _recGroup.Text = "Запись эфира";
        _recGroup.Name = "recGroup";
        _recGroup.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        _recPathLabel.Location = new Point(16, 28);
        _recPathLabel.Size = new Size(45, 22);
        _recPathLabel.Text = "Файл:";
        _recPathLabel.TextAlign = ContentAlignment.MiddleLeft;
        _recPathLabel.Name = "recPathLabel";

        _recPathText.Location = new Point(65, 26);
        _recPathText.Size = new Size(512, 24);
        _recPathText.Name = "recPathText";
        _recPathText.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        // В4: пусто = дефолт хоста (Videos\VCam_*.mp4 в момент старта);
        // пример виден серым до первого ApplySettingsToControls.
        _recPathText.PlaceholderText = DefaultRecPath();
        _recPathText.TextChanged += (_, _) => MarkDirty();

        _recBrowse.Location = new Point(587, 25);
        _recBrowse.Size = new Size(100, 28);
        _recBrowse.Text = "Обзор…";
        _recBrowse.Name = "recBrowse";
        _recBrowse.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        _recBrowse.Click += OnRecBrowseClicked;

        _recButton.Location = new Point(697, 25);
        _recButton.Size = new Size(143, 28);
        _recButton.Text = "● Начать запись";
        _recButton.Name = "recButton";
        _recButton.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        _recButton.Click += OnRecButtonClicked;

        _recStatus.Location = new Point(16, 57);
        _recStatus.Size = new Size(824, 22);
        _recStatus.Text = "Не записывается";
        _recStatus.ForeColor = Color.DimGray;
        _recStatus.Name = "recStatus";
        // М3: длинный путь записи обрезается с "…" как у соседних
        // _videoPathLabel/_pathLabel/_cameraIdLabel (без этого хвост просто
        // клиппился без индикации).
        _recStatus.AutoEllipsis = true;
        _recStatus.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        _recHint.Location = new Point(16, 79);
        _recHint.Size = new Size(700, 22);
        _recHint.ForeColor = Color.DimGray;
        _recHint.Name = "recHint";
        _recHint.Anchor = AnchorStyles.Top | AnchorStyles.Left;

        _recHotkeyEdit.Location = new Point(730, 76);
        _recHotkeyEdit.Size = new Size(110, 28);
        _recHotkeyEdit.Text = "Изменить…";
        _recHotkeyEdit.Name = "recHotkeyEditButton";
        _recHotkeyEdit.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        _recHotkeyEdit.Click += OnRecHotkeyEditClicked;

        _recGroup.Controls.AddRange(new Control[] {
            _recPathLabel, _recPathText, _recBrowse, _recButton, _recStatus, _recHint, _recHotkeyEdit });
    }

    // Any user edit after Load/Save marks the form dirty; while dirty the
    // auto-reload from settings.json is blocked (warning instead) so external
    // changes never silently discard what the user is editing.
    private void MarkDirty()
    {
        if (_suppressDirty || _dirty) return;
        _dirty = true;
    }

    private void SubscribeDirtyTracking()
    {
        foreach (var c in new[] { _cropKeepAspect })
            c.CheckedChanged += (_, _) => MarkDirty();
        _mode.SelectedIndexChanged += (_, _) => MarkDirty();
        _mediaCombo.SelectedIndexChanged += (_, _) => MarkDirty();
        _qualityCombo.SelectedIndexChanged += (_, _) => MarkDirty();
        _captureCombo.SelectedIndexChanged += (_, _) => MarkDirty();
        _cameraCombo.SelectedIndexChanged += (_, _) => MarkDirty();
        // Crop fields: programmatic sync runs under _updatingCropFields.
        foreach (var n in new[] { _cropX, _cropY, _cropW, _cropH })
            n.ValueChanged += (_, _) => { if (!_updatingCropFields) MarkDirty(); };
        // Crop rectangle drags and file picks go through OnCropSelectionChanged
        // / SetSource / OnOpenClicked (each marks dirty there).
    }

    private void InitSettingsSync()
    {
        _lastAppliedText = ReadSettingsText();
        _syncTimer.Interval = 250;
        _syncTimer.Tick += OnSyncTimerTick;
        try
        {
            Directory.CreateDirectory(Settings.DirectoryPath);
            _settingsWatcher = new FileSystemWatcher(Settings.DirectoryPath,
                Path.GetFileName(Settings.FilePath))
            {
                NotifyFilter = NotifyFilters.LastWrite | NotifyFilters.Size |
                               NotifyFilters.CreationTime,
                EnableRaisingEvents = true,
            };
            _settingsWatcher.Changed += OnSettingsFileChanged;
            _settingsWatcher.Created += OnSettingsFileChanged;
            _settingsWatcher.Renamed += OnSettingsFileChanged;
            _settingsWatcher.Deleted += OnSettingsFileChanged;
            // Buffer overflow: the next save/manual reload converges anyway.
            _settingsWatcher.Error += (_, _) => { };
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"MainForm: settings watcher disabled: {ex.Message}");
            _settingsWatcher = null;
        }
    }

    // FileSystemWatcher fires on a pool thread (possibly several bursts per
    // save): marshal to the UI thread and restart the debounce timer.
    private void OnSettingsFileChanged(object? sender, FileSystemEventArgs e)
    {
        if (IsDisposed || Disposing) return;
        try
        {
            BeginInvoke((Action)(() =>
            {
                if (IsDisposed || Disposing) return;
                _syncTimer.Stop();
                _syncTimer.Start();
            }));
        }
        catch (ObjectDisposedException) { }
        catch (InvalidOperationException) { } // handle not created yet
    }

    private void OnSyncTimerTick(object? sender, EventArgs e)
    {
        _syncTimer.Stop();
        TrySyncFromFile();
    }

    private void RetrySyncLater()
    {
        // Transient read (half-written file, lock): retry a few times, then
        // give up quietly — the next external change or manual reload retries.
        if (_syncRetries++ < 8)
        {
            _syncTimer.Stop();
            _syncTimer.Start();
        }
        else
        {
            _syncRetries = 0;
        }
    }

    private static string ReadSettingsText()
    {
        try
        {
            return File.Exists(Settings.FilePath)
                ? File.ReadAllText(Settings.FilePath)
                : "";
        }
        catch
        {
            return "";
        }
    }

    private void TrySyncFromFile()
    {
        string text;
        try
        {
            if (!File.Exists(Settings.FilePath)) return; // deleted: keep controls
            text = File.ReadAllText(Settings.FilePath);
        }
        catch (IOException)
        {
            RetrySyncLater();
            return;
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"MainForm: settings re-read failed: {ex.Message}");
            return;
        }

        if (string.IsNullOrWhiteSpace(text))
        {
            RetrySyncLater(); // truncated mid-write: wait for the rest
            return;
        }

        // Echo suppression by content, not by timing: our own Save (and any
        // spurious duplicate event) yields exactly the snapshot the controls
        // already reflect, so there is nothing to do.
        if (text == _lastAppliedText)
        {
            _syncRetries = 0;
            return;
        }

        // Never apply half-written JSON: Settings.Load() fails soft to
        // defaults, which would blank the controls on a torn read.
        try
        {
            using var doc = JsonDocument.Parse(text);
        }
        catch (JsonException)
        {
            RetrySyncLater();
            return;
        }

        if (_dirty)
        {
            // Guard unsaved user edits: keep the controls, show the indicator.
            _reloadButton.Text = "Обновить *";
            _hintLabel.ForeColor = Color.DarkGoldenrod;
            _hintLabel.Text = "Файл настроек изменён извне — ваши несохранённые правки " +
                "не тронуты. Нажмите «Обновить», чтобы загрузить файл, или " +
                "«Сохранить настройки», чтобы перезаписать его.";
            return;
        }

        ApplySettingsText(text);
    }

    // Applies a validated snapshot to all controls (same mapping as startup).
    private void ApplySettingsText(string text)
    {
        Settings s;
        try
        {
            s = Settings.LoadFromText(text);
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"MainForm: settings parse failed: {ex.Message}");
            RetrySyncLater();
            return;
        }

        _suppressDirty = true;
        try
        {
            ApplySettingsToControls(s);
            _dirty = false;
            _syncRetries = 0;
            _reloadButton.Text = "Обновить";
            _lastAppliedText = text;
            _hintLabel.ForeColor = Color.ForestGreen;
            _hintLabel.Text = $"Настройки обновлены из файла ({DateTime.Now:HH:mm:ss}) — " +
                "хост подхватит их автоматически (~1 с).";
        }
        finally
        {
            _suppressDirty = false;
        }
        UpdateLayout();
    }

    private void OnReloadClicked(object? sender, EventArgs e)
    {
        // Explicit user choice, no extra confirmation: the flagged "Обновить *"
        // is only shown after the external-change warning (edits knowingly
        // discarded); with no pending change a reload is a no-op anyway.
        _syncTimer.Stop();
        var text = ReadSettingsText();
        if (string.IsNullOrWhiteSpace(text))
        {
            _hintLabel.ForeColor = Color.DarkGoldenrod;
            _hintLabel.Text = "Не удалось прочитать файл настроек — повторите позже.";
            return;
        }
        if (text == _lastAppliedText)
        {
            _reloadButton.Text = "Обновить";
            _hintLabel.ForeColor = Color.DimGray;
            _hintLabel.Text = $"Настройки: {Settings.FilePath} — уже актуальны.";
            return;
        }
        ApplySettingsText(text);
    }

    private static void PlaceCropField(Label label, NumericUpDown input, int x, int y, string text)
    {
        label.Location = new Point(x, y + 4);
        label.Size = new Size(text == "X" || text == "Y" ? 16 : 56, 18);
        label.Text = text;
        input.Location = new Point(x + label.Size.Width + 4, y);
        input.Size = new Size(72, 24);
        input.Maximum = 100000;
    }

    // VCamPreview.exe lookup: (a) next to VCamSettingsUi.exe, (b) <repo>\build\x64\Release
    // while walking up from AppContext.BaseDirectory (depth varies with the output layout).
    private static string? FindPreviewExe()
    {
        var baseDir = AppContext.BaseDirectory;
        var direct = Path.Combine(baseDir, "VCamPreview.exe");
        if (File.Exists(direct)) return direct;
        var dir = new DirectoryInfo(baseDir);
        for (var i = 0; i < 10 && dir != null; i++, dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "build", "x64", "Release", "VCamPreview.exe");
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    // VCamVideoStreamProducer.exe lookup: (a) next to VCamSettingsUi.exe,
    // (b) <walked-up root>\build\x64\Release.
    private static string? FindHostExe()
    {
        var baseDir = AppContext.BaseDirectory;
        var direct = Path.Combine(baseDir, "VCamVideoStreamProducer.exe");
        if (File.Exists(direct)) return direct;
        var dir = new DirectoryInfo(baseDir);
        for (var i = 0; i < 10 && dir != null; i++, dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "build", "x64", "Release", "VCamVideoStreamProducer.exe");
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    // VCamProducerCli.exe lookup, same pattern as VCamPreview.exe:
    // (a) next to VCamSettingsUi.exe, (b) <walked-up root>\build\x64\Release.
    private static string? FindCliExe()
    {
        var baseDir = AppContext.BaseDirectory;
        var direct = Path.Combine(baseDir, "VCamProducerCli.exe");
        if (File.Exists(direct)) return direct;
        var dir = new DirectoryInfo(baseDir);
        for (var i = 0; i < 10 && dir != null; i++, dir = dir.Parent)
        {
            var candidate = Path.Combine(dir.FullName, "build", "x64", "Release", "VCamProducerCli.exe");
            if (File.Exists(candidate)) return candidate;
        }
        return null;
    }

    // The host owns the named mutex (WaitOne(0) times out while it runs); an
    // abandoned/free mutex or a missing one means "not running".
    private static bool IsHostRunning()
    {
        try
        {
            using var mutex = Mutex.OpenExisting(HostMutexName);
            if (mutex.WaitOne(0))
            {
                mutex.ReleaseMutex();
                return false;
            }
            return true; // WAIT_TIMEOUT: owned by the host
        }
        catch (WaitHandleCannotBeOpenedException)
        {
            return false;
        }
        catch (Exception)
        {
            return false;
        }
    }

    private void UpdateHostStatus()
    {
        var running = IsHostRunning();
        _hostStatusLabel.Text = running ? "Хост: запущен" : "Хост: не запущен";
        _hostStatusLabel.ForeColor = running ? Color.ForestGreen : Color.DimGray;
        _hostButton.Text = running ? "Перезапустить хост" : "Запустить хост";
        UpdateHotkeyBorrowLabel(); // опрос transient borrow-состояния (1 с)
        UpdateRecStatus(); // опрос transient состояния записи (1 с)
    }

    // В1: ожидание graceful-выхода (до 5 с) — в Task.Run, форму не вешает:
    // кнопка disabled + «перезапускается…», продолжение на UI-потоке.
    private async void OnHostButtonClicked(object? sender, EventArgs e)
    {
        if (IsHostRunning())
        {
            try
            {
                using var stop = EventWaitHandle.OpenExisting(HostStopEventName);
                stop.Set(); // SetEvent, не Kill: хост закрывает writer сам
            }
            catch (WaitHandleCannotBeOpenedException)
            {
                MessageBox.Show(this, "Событие остановки хоста не найдено — хост уже завершён.", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                UpdateHostStatus();
                return;
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Не удалось остановить хост:\n{ex.Message}", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return;
            }

            // Перезапуск: дождаться graceful-выхода (обычно ~0.2 с), иначе новый
            // экземпляр упрётся в single-instance мьютекс и покажет «уже запущен».
            _hostButton.Enabled = false;
            _hostStatusLabel.Text = "Хост: перезапускается…";
            var exited = await Task.Run(() =>
            {
                var deadline = Environment.TickCount64 + 5000;
                while (IsHostRunning() && Environment.TickCount64 < deadline)
                    Thread.Sleep(100);
                return !IsHostRunning();
            });
            if (IsDisposed) return;
            _hostButton.Enabled = true;
            if (!exited)
            {
                MessageBox.Show(this, "Хост не завершился за 5 с — перезапуск отменён.", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                UpdateHostStatus();
                return;
            }
        }

        if (_hostExe is null || !File.Exists(_hostExe))
        {
            MessageBox.Show(this,
                "VCamVideoStreamProducer.exe не найден: искал рядом с VCamSettingsUi.exe и в " +
                "<корень репозитория>\\build\\x64\\Release.",
                Text, MessageBoxButtons.OK, MessageBoxIcon.Warning);
            return;
        }

        try
        {
            Process.Start(new ProcessStartInfo(_hostExe)
            {
                WorkingDirectory = Path.GetDirectoryName(_hostExe) ?? AppContext.BaseDirectory,
            });
            _hostStatusLabel.Text = "Хост: запускается…";
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось запустить хост:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void LoadCurrentSettings()
    {
        var s = Settings.Load();
        _suppressDirty = true;
        try
        {
            ApplySettingsToControls(s);
        }
        finally
        {
            _suppressDirty = false;
        }
        _dirty = false;
        _lastAppliedText = ReadSettingsText();
    }

    // One mapping file -> controls, shared by startup, auto-reload and manual
    // reload. Must run under _suppressDirty (programmatic sets, not edits).
    private void ApplySettingsToControls(Settings s)
    {
        // В3: программная синхронизация — не пользовательский выбор.
        _mediaChangedByUser = false;
        _cropKeepAspect.Checked = s.CropKeepAspect;
        _hotkeyMods = s.HotkeyModifiers;
        _hotkeyVk = s.HotkeyVk;
        _recHotkeyMods = s.RecordHotkeyModifiers;
        _recHotkeyVk = s.RecordHotkeyVk;
        _videoHotkeyMods = s.VideoHotkeyModifiers;
        _videoHotkeyVk = s.VideoHotkeyVk;
        UpdateVideoHotkeyHint();
        _videoLoopCheck.Checked = s.VideoLoop;
        UpdateHotkeyHint();
        UpdateHotkeyBorrowLabel();
        // В4: пустой record.path показываем пустым боксом (имя-файл генерится
        // только в момент старта записи хостом), пример — серым плейсхолдером.
        // Раньше сюда подставлялся DefaultRecPath() с текущим timestamp и он
        // же сохранялся в settings — файл запоминал мусорное имя.
        _recPathText.Text = s.RecordPath ?? "";
        _recPathText.PlaceholderText = DefaultRecPath();
        UpdateRecHint();
        _mode.SelectedIndex = s.ScaleMode switch
        {
            ScaleMode.Cover => 1,
            ScaleMode.Crop => 2,
            _ => 0,
        };
        _videoPath = s.VideoPath; // set before the combo: switching fires UpdateLayout
        _cameraWishId = s.CameraId;   // remembered device; applied when the list loads
        _cameraWishName = s.CameraName;
        _captureCombo.SelectedIndex = s.Capture switch
        {
            CaptureMode.P720 => 1,
            CaptureMode.P1080 => 2,
            _ => 0,
        };
        _qualityCombo.SelectedIndex = s.Quality switch
        {
            Quality.Fixed720p => 1,
            Quality.Fixed1080p => 2,
            _ => 0,
        };
        _mediaCombo.SelectedIndex = s.SourceType switch
        {
            // В3: неизвестный будущий токен показываем как static (комбо его
            // не умеет), но в файл он вернётся verbatim (см. Collect +
            // _mediaChangedByUser) — как C++ хранит verbatim.
            SourceTypes.Video => 1,
            SourceTypes.Camera => 2,
            _ => 0,
        };

        // The static panel edits the "static" section; it is loaded in both modes
        // so switching back to "статичная картинка" keeps working.
        if (!string.IsNullOrEmpty(s.StaticPath) && File.Exists(s.StaticPath))
        {
            SetSource(s.StaticPath, new Rectangle(s.CropX, s.CropY, s.CropW, s.CropH));
        }
        else
        {
            // Missing file: remember the path anyway so a later save writes
            // exactly what is on disk, and drop the stale picture so the
            // preview never lies.
            _sourcePath = s.StaticPath ?? "";
            _cropView.Image = null;
            if (_sourceImage is not null)
            {
                _sourceImage.Dispose();
                _sourceImage = null;
            }
            var oldPreview = _preview.Image;
            _preview.Image = null;
            oldPreview?.Dispose();
            _fullSizeButton.Enabled = false;
        }

        // External change may rename the remembered camera: re-select by id
        // when the list is already loaded (FillCameraCombo suppresses dirty).
        if (_cameraListLoaded)
            FillCameraCombo();
        UpdateCameraPanel();
    }

    private ScaleMode CurrentMode => _mode.SelectedIndex switch
    {
        1 => ScaleMode.Cover,
        2 => ScaleMode.Crop,
        _ => ScaleMode.Fit,
    };

    private string CurrentSourceType => _mediaCombo.SelectedIndex switch
    {
        1 => SourceTypes.Video,
        2 => SourceTypes.Camera,
        _ => SourceTypes.Static,
    };

    private Quality CurrentQuality => _qualityCombo.SelectedIndex switch
    {
        1 => Quality.Fixed720p,
        2 => Quality.Fixed1080p,
        _ => Quality.Source,
    };

    // DropDownList => index always 0..2; anything unexpected maps to Max.
    private CaptureMode CurrentCapture => _captureCombo.SelectedIndex switch
    {
        1 => CaptureMode.P720,
        2 => CaptureMode.P1080,
        _ => CaptureMode.Max,
    };

    // Single source of truth for control visibility (media mode x scale mode).
    private void UpdateLayout()
    {
        bool video = CurrentSourceType == SourceTypes.Video;
        bool camera = CurrentSourceType == SourceTypes.Camera;
        bool crop = !video && !camera && CurrentMode == ScaleMode.Crop;

        _videoPanel.Visible = video;
        _cameraPanel.Visible = camera;
        _preview.Visible = !video && !camera && !crop;
        _cropView.Visible = crop;
        foreach (var c in new Control[] { _mode, _fullSizeButton })
            c.Visible = !video && !camera;
        foreach (var c in new Control[] { _cropXLabel, _cropX, _cropYLabel, _cropY, _cropWLabel, _cropW, _cropHLabel, _cropH, _cropKeepAspect })
            c.Visible = crop;

        _openButton.Visible = !camera;
        _pathLabel.Visible = !camera;
        _openButton.Text = video ? "Выбрать видео…" : "Открыть картинку…";
        _pathLabel.Text = video
            ? (string.IsNullOrEmpty(_videoPath) ? "(ролик не выбран)" : _videoPath)
            : (string.IsNullOrEmpty(_sourcePath) ? "(файл не выбран)" : _sourcePath);

        if (camera)
        {
            EnsureCameraList(); // lazy: enumerate on the first switch to camera
            UpdateCameraPanel();
        }
        else if (video)
        {
            UpdateVideoPanel();
        }
        else if (crop)
        {
            if (_sourceImage != null && _cropView.Image == null)
            {
                _cropView.Image = _sourceImage; // sets default selection + fires SelectionChanged
            }
            UpdateCropFieldsFromSelection();
        }
        else
        {
            UpdatePreview();
        }
    }

    private void UpdateVideoPanel()
    {
        _videoPathLabel.Text = string.IsNullOrEmpty(_videoPath)
            ? "(ролик не выбран — нажмите «Выбрать видео…»)"
            : _videoPath;
        bool found = _previewExe != null;
        _previewButton.Enabled = found;
        _previewHint.Text = found
            ? $"Окно предпросмотра: {_previewExe}\r\n" +
              "Запускается отдельным процессом, поверх всех окон (always-on-top). Esc — выход."
            : "VCamPreview.exe не найден: искал рядом с VCamSettingsUi.exe и в " +
              "<корень репозитория>\\build\\x64\\Release — окно предпросмотра недоступно, кнопка отключена.";
        UpdateHotkeyBorrowLabel();
    }

    // Transient hotkey-borrow state the host writes while a hotkey-borrowed
    // clip is on air (%APPDATA%\VCam\hotkey_state.json, NOT settings.json —
    // otherwise the settings watcher would loop the switches). Missing file
    // (or garbage) = no borrow. Never throws.
    private static string HotkeyStatePath =>
        Path.Combine(Settings.DirectoryPath, "hotkey_state.json");

    private static bool TryReadHotkeyBorrow(out string returnTo)
    {
        returnTo = "";
        string text;
        try
        {
            if (!File.Exists(HotkeyStatePath)) return false;
            text = File.ReadAllText(HotkeyStatePath);
        }
        catch
        {
            return false;
        }
        try
        {
            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return false;
            var borrowed = root.TryGetProperty("borrowed", out var b) &&
                           b.ValueKind == JsonValueKind.True;
            if (!borrowed) return false;
            if (root.TryGetProperty("returnTo", out var r) &&
                r.ValueKind == JsonValueKind.String)
                returnTo = r.GetString() ?? "";
            return true;
        }
        catch
        {
            return false;
        }
    }

    // Mirrors the host HotkeyDisplay (C++): modifiers are RegisterHotKey bits
    // (1=Alt, 2=Ctrl, 4=Shift, 8=Win), vk is the Virtual-Key code. Garbage is
    // already normalised to Ctrl+Alt+V by Settings parsing on both sides.
    private static string HotkeyDisplayMods(int mods, int vk)
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

    // Hotkey editors: modal capture (HotkeyEditForm), values go to
    // settings.json on «Сохранить»; the host re-registers live (~1 с).
    private void OnHotkeyEditClicked(object? sender, EventArgs e)
    {
        using var dlg = new HotkeyEditForm("Хоткей — показать видео один раз", _hotkeyMods, _hotkeyVk);
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        _hotkeyMods = dlg.Modifiers;
        _hotkeyVk = dlg.Vk;
        MarkDirty();
        UpdateHotkeyHint();
    }

    private void OnRecHotkeyEditClicked(object? sender, EventArgs e)
    {
        using var dlg = new HotkeyEditForm("Хоткей — старт/стоп записи", _recHotkeyMods, _recHotkeyVk);
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        _recHotkeyMods = dlg.Modifiers;
        _recHotkeyVk = dlg.Vk;
        MarkDirty();
        UpdateRecHint();
    }

    private void OnVideoHotkeyEditClicked(object? sender, EventArgs e)
    {
        using var dlg = new HotkeyEditForm("Хоткей — play/pause видео", _videoHotkeyMods, _videoHotkeyVk);
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        _videoHotkeyMods = dlg.Modifiers;
        _videoHotkeyVk = dlg.Vk;
        MarkDirty();
        UpdateVideoHotkeyHint();
    }

    // videoHotkey hint line (video panel): current play/pause combination.
    private void UpdateVideoHotkeyHint()
    {
        _videoHotkeyHint.Text = "Play/Pause: " + HotkeyDisplayMods(_videoHotkeyMods, _videoHotkeyVk) +
            " — пауза/возобновление, после конца ролика — сначала.";
    }

    // Always-visible hotkey line: current combination + what it does.
    // В5: автовозврат — и по концу ролика, и при обрыве (битый файл уводит
    // хост в fallback — borrow снимается там же, а не висит вечно).
    private void UpdateHotkeyHint()
    {
        _hotkeyHint.Text = "Горячая клавиша: " + HotkeyDisplayMods(_hotkeyMods, _hotkeyVk) +
            " — показать видео один раз (повторно — вернуться сразу; " +
            "после конца ролика — автовозврат, при битом файле — возврат сразу). Комбинация — кнопкой «Изменить…».";
    }

    // Borrowed-video indicator, polled (host timer tick + panel updates): while
    // the host holds a hotkey borrow the label names the source the clip will
    // return to; otherwise it stays a quiet hint.
    private void UpdateHotkeyBorrowLabel()
    {
        if (TryReadHotkeyBorrow(out var returnTo))
        {
            if (string.IsNullOrEmpty(returnTo)) returnTo = "static";
            var borrowText =
                "▶ Сейчас идёт видео по горячей клавише: после конца ролика " +
                $"эфир вернётся на «{returnTo}» (повторное нажатие — вернуться сразу).";
            if (_hotkeyBorrowLabel.Text != borrowText)
            {
                _hotkeyBorrowLabel.ForeColor = Color.DarkRed;
                _hotkeyBorrowLabel.Text = borrowText;
            }
            const string suffix = " Сейчас идёт видео по горячей клавише.";
            if (!_hotkeyHint.Text.EndsWith(suffix, StringComparison.Ordinal))
                _hotkeyHint.Text += suffix;
        }
        else
        {
            const string idleText =
                "Видео по горячей клавише будет играть один раз — " +
                "после конца ролика эфир вернётся сам.";
            if (_hotkeyBorrowLabel.Text != idleText)
            {
                _hotkeyBorrowLabel.ForeColor = Color.DimGray;
                _hotkeyBorrowLabel.Text = idleText;
            }
            const string suffix = " Сейчас идёт видео по горячей клавише.";
            if (_hotkeyHint.Text.EndsWith(suffix, StringComparison.Ordinal))
                _hotkeyHint.Text = _hotkeyHint.Text[..^suffix.Length];
        }
    }

    // Transient record state the host writes while recording
    // (%APPDATA%\VCam\record_state.json, NOT settings.json — otherwise the
    // settings watcher would loop start/stop). Missing file (or garbage) =
    // not recording. Never throws.
    private static string RecordStatePath =>
        Path.Combine(Settings.DirectoryPath, "record_state.json");

    private static string RecordCommandPath =>
        Path.Combine(Settings.DirectoryPath, "record_command.json");

    private static bool TryReadRecordState(out string path, out long started)
    {
        path = "";
        started = 0;
        string text;
        try
        {
            if (!File.Exists(RecordStatePath)) return false;
            text = File.ReadAllText(RecordStatePath);
        }
        catch
        {
            return false;
        }
        try
        {
            using var doc = JsonDocument.Parse(text);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return false;
            var rec = root.TryGetProperty("recording", out var b) &&
                      b.ValueKind == JsonValueKind.True;
            if (!rec) return false;
            if (root.TryGetProperty("path", out var p) && p.ValueKind == JsonValueKind.String)
                path = p.GetString() ?? "";
            if (root.TryGetProperty("started", out var st) && st.ValueKind == JsonValueKind.Number &&
                st.TryGetInt64(out var unix))
                started = unix;
            return true;
        }
        catch
        {
            return false;
        }
    }

    // Default record path (mirrors the host DefaultRecordPath): Videos folder
    // + VCam_yyyyMMdd_HHmmss.mp4. The host generates the same when the box
    // (and settings record.path) is empty.
    // В7: единый резолв — Environment.SpecialFolder.MyVideos это и есть
    // SHGetKnownFolderPath(FOLDERID_Videos) (как теперь в хосте); fallback
    // %USERPROFILE%\Videos — та же цепочка, что у хоста.
    private static string DefaultRecPath()
    {
        string videos;
        try
        {
            videos = Environment.GetFolderPath(Environment.SpecialFolder.MyVideos);
        }
        catch
        {
            videos = "";
        }
        if (string.IsNullOrEmpty(videos))
            videos = Path.Combine(
                Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Videos");
        return Path.Combine(videos, $"VCam_{DateTime.Now:yyyyMMdd_HHmmss}.mp4");
    }

    // Command UI -> host (start/stop): atomic tmp+move so the worker never
    // reads a torn file. Throws with a readable message on failure.
    // Relaxed escaping keeps Cyrillic paths as UTF-8: the host decodes
    // \uXXXX as '?' but passes UTF-8 bytes through.
    private static void WriteRecordCommand(string cmd, string path)
    {
        var cmdPath = RecordCommandPath;
        var dir = Path.GetDirectoryName(cmdPath);
        if (!string.IsNullOrEmpty(dir)) Directory.CreateDirectory(dir);
        var options = new JsonSerializerOptions
        {
            Encoder = System.Text.Encodings.Web.JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        };
        var payload = JsonSerializer.Serialize(new Dictionary<string, string>
        {
            ["cmd"] = cmd,
            ["path"] = path,
        }, options);
        var tmp = cmdPath + ".tmp";
        File.WriteAllText(tmp, payload, new UTF8Encoding(false));
        File.Move(tmp, cmdPath, true);
    }

    private void UpdateRecHint()
    {
        _recHint.Text = "Горячая клавиша записи: " + HotkeyDisplayMods(_recHotkeyMods, _recHotkeyVk) +
            " — старт/стоп (комбинация — кнопкой «Изменить…»).";
    }

    // REC indicator, polled on the host timer tick (1 s): red dot + elapsed
    // + file while recording, quiet line otherwise. Button text follows.
    // Text is only reassigned on change (no flicker).
    private void UpdateRecStatus()
    {
        if (TryReadRecordState(out var path, out var started))
        {
            _recRecording = true;
            if (!string.IsNullOrEmpty(path)) _lastRecPath = path;
            var el = DateTimeOffset.UtcNow.ToUnixTimeSeconds() - started;
            if (el < 0) el = 0;
            var status = $"● REC {el / 60:D2}:{el % 60:D2} — {path}";
            if (_recStatus.Text != status)
            {
                _recStatus.ForeColor = Color.Red;
                _recStatus.Text = status;
            }
            const string stop = "■ Остановить";
            if (_recButton.Text != stop) _recButton.Text = stop;
        }
        else
        {
            _recRecording = false;
            var status = string.IsNullOrEmpty(_lastRecPath)
                ? "Не записывается"
                : $"Не записывается. Последний файл: {_lastRecPath}";
            if (_recStatus.Text != status)
            {
                _recStatus.ForeColor = Color.DimGray;
                _recStatus.Text = status;
            }
            const string start = "● Начать запись";
            if (_recButton.Text != start) _recButton.Text = start;
        }
    }

    private void OnRecBrowseClicked(object? sender, EventArgs e)
    {
        using var dlg = new SaveFileDialog();
        dlg.Filter = "Видео MP4 (*.mp4)|*.mp4|Все файлы (*.*)|*.*";
        dlg.DefaultExt = "mp4";
        var cur = _recPathText.Text.Trim();
        try
        {
            if (!string.IsNullOrEmpty(cur))
            {
                var dir = Path.GetDirectoryName(cur);
                if (!string.IsNullOrEmpty(dir) && Directory.Exists(dir))
                    dlg.InitialDirectory = dir;
                dlg.FileName = Path.GetFileName(cur);
            }
            else
            {
                dlg.InitialDirectory = Path.GetDirectoryName(DefaultRecPath());
                dlg.FileName = Path.GetFileName(DefaultRecPath());
            }
        }
        catch
        {
            // Диалог и так подставит разумное.
        }
        if (dlg.ShowDialog(this) == DialogResult.OK)
            _recPathText.Text = dlg.FileName; // TextChanged -> MarkDirty
    }

    private void OnRecButtonClicked(object? sender, EventArgs e)
    {
        if (_recRecording)
        {
            try
            {
                WriteRecordCommand("stop", "");
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Не удалось остановить запись:\n{ex.Message}", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
            }
            return;
        }
        if (!IsHostRunning())
        {
            MessageBox.Show(this, "Хост не запущен — запись некому вести. Нажмите «Запустить хост».", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            UpdateHostStatus();
            return;
        }
        // В4: пустой бокс = пустая строка (дефолт хоста), имя генерится только
        // в момент старта записи (хост StartRecording резолвит пусто в
        // Videos\VCam_*.mp4 и сам дописывает его в settings). Раньше UI тут
        // изобретал timestamp-имя и клал его в бокс+settings заранее.
        var path = _recPathText.Text.Trim();
        // Лёгкий Save только record.path поверх диска (без валидации
        // source-секций из CollectSettingsFromControls — старт записи не
        // должен упираться в незаполненный источник).
        // К1: битый settings.json НЕ затираем дефолтами — читаем строгим
        // LoadFromText и отменяем старт при исключении.
        try
        {
            var liveText = ReadSettingsText();
            Settings s;
            if (string.IsNullOrWhiteSpace(liveText))
            {
                s = new Settings(); // файла ещё нет — первый старт
            }
            else
            {
                try
                {
                    s = Settings.LoadFromText(liveText);
                }
                catch (Exception parseEx)
                {
                    MessageBox.Show(this, $"Файл настроек повреждён — путь записи не сохранён, запись не начата:\n{parseEx.Message}", Text,
                        MessageBoxButtons.OK, MessageBoxIcon.Error);
                    return;
                }
            }
            s.RecordPath = path;
            s.Save();
            _dirty = false;
            _syncRetries = 0;
            _reloadButton.Text = "Обновить";
            _lastAppliedText = ReadSettingsText();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось сохранить путь записи:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }
        try
        {
            WriteRecordCommand("start", path);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось начать запись:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    // First switch to the camera source (also at startup when the saved type is
    // "camera"): run the CLI enumeration once; failures never crash the form.
    // В1: запуск — fire-and-forget (форма не висит, список подъедет сам).
    private void EnsureCameraList()
    {
        if (_cameraListLoaded) return;
        _cameraListLoaded = true;
        _ = LoadCameraListAsync();
    }

    private bool _cameraLoading; // реентрантность кнопки «Обновить список»

    // "VCamProducerCli list-devices": stdout = rows "<id>\t<name>" (UTF-8),
    // the header goes to stderr and is ignored by the parser. Exit code is
    // always 0 (including zero devices).
    // В1: было синхронно в UI-потоке (WaitForExit 8 с вешал форму).
    // Теперь CLI крутится в Task.Run, форма жива: кнопка disabled + статус
    // «получение…», результат применяется обратно на UI-потоке.
    private async Task LoadCameraListAsync()
    {
        if (_cameraLoading) return;
        _cameraLoading = true;
        _cameraRefresh.Enabled = false;
        _cameraListStatus = "Получение списка камер…";
        UpdateCameraStatus(null);
        try
        {
            _cliExe ??= FindCliExe();
            if (_cliExe is null)
            {
                _cameraListStatus = "VCamProducerCli.exe не найден: искал рядом с VCamSettingsUi.exe и в " +
                    "<корень репозитория>\\build\\x64\\Release — список камер недоступен, выберите «Обновить список» после установки.";
            }
            else
            {
                var cli = _cliExe;
                var (ok, stdout, stderr, fail) = await Task.Run(() => RunListDevices(cli));
                if (IsDisposed) return;
                if (!ok)
                {
                    _cameraListStatus = fail;
                }
                else
                {
                    _cameraItems.Clear();
                    ParseListDevices(stdout);
                    var err = stderr.Trim();
                    _cameraListStatus = _cameraItems.Count > 0
                        ? $"Камер найдено: {_cameraItems.Count}."
                        : "Камеры не найдены (0)." + (err.Length > 0 ? $" {err}" : "");
                }
            }
        }
        finally
        {
            _cameraLoading = false;
            if (!IsDisposed)
            {
                _cameraRefresh.Enabled = true;
                FillCameraCombo();
                UpdateCameraStatus(null);
                UpdateCameraPanel();
            }
        }
    }

    // Синхронный прогон CLI (только из Task.Run, НЕ из UI-потока):
    // дренаж обоих пайпов + WaitForExit(8 с) как раньше.
    private static (bool Ok, string Stdout, string Stderr, string Fail) RunListDevices(string cliExe)
    {
        try
        {
            using var proc = Process.Start(new ProcessStartInfo(cliExe)
            {
                Arguments = "list-devices",
                UseShellExecute = false,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
                StandardOutputEncoding = Encoding.UTF8,
                StandardErrorEncoding = Encoding.UTF8,
                CreateNoWindow = true,
                WorkingDirectory = Path.GetDirectoryName(cliExe) ?? AppContext.BaseDirectory,
            });
            if (proc is null)
                return (false, "", "", "Не удалось запустить VCamProducerCli — список камер недоступен.");
            // Drain both pipes on background tasks so a full stderr buffer
            // cannot deadlock the wait below.
            var outTask = proc.StandardOutput.ReadToEndAsync();
            var errTask = proc.StandardError.ReadToEndAsync();
            var exited = proc.WaitForExit(8000);
            if (!exited)
            {
                try { proc.Kill(); } catch { /* already gone */ }
                return (false, "", "", "VCamProducerCli list-devices не завершился за 8 с — список не получен.");
            }
            return (true, outTask.Result, errTask.Result, "");
        }
        catch (Exception ex)
        {
            return (false, "", "", $"Не удалось получить список камер: {ex.Message}");
        }
    }

    private void ParseListDevices(string stdout)
    {
        foreach (var raw in stdout.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0) continue;
            var tab = line.IndexOf('\t');
            if (tab <= 0) continue;
            var id = line[..tab].Trim();
            var name = line[(tab + 1)..].Trim();
            if (id.Length == 0) continue;
            _cameraItems.Add(new CameraItem(id, name.Length == 0 ? id : name, missing: false));
        }
    }

    // Rebuild the ComboBox keeping the current selection: by id first, then by
    // name; a remembered device that is missing now is appended as
    // "<name> (недоступно)" so the saved choice is never lost.
    private void FillCameraCombo()
    {
        // Programmatic re-selection (list load/refresh, external reload) must
        // not look like a user edit: suppress the dirty flag, restoring the
        // outer state (reload runs suppressed already, refresh does not).
        var prev = _suppressDirty;
        _suppressDirty = true;
        try
        {
            FillCameraComboCore();
        }
        finally
        {
            _suppressDirty = prev;
        }
    }

    private void FillCameraComboCore()
    {
        var current = _cameraCombo.SelectedItem as CameraItem;
        var keepId = current?.Id ?? _cameraWishId;
        var keepName = current?.Name ?? _cameraWishName;

        _cameraCombo.Items.Clear();
        foreach (var item in _cameraItems)
            _cameraCombo.Items.Add(item);

        CameraItem? selected = null;
        if (keepId.Length > 0)
            selected = _cameraItems.Find(i => string.Equals(i.Id, keepId, StringComparison.OrdinalIgnoreCase));
        if (selected is null && keepName.Length > 0)
            selected = _cameraItems.Find(i => string.Equals(i.Name, keepName, StringComparison.OrdinalIgnoreCase));
        if (selected is null && keepId.Length > 0)
        {
            selected = new CameraItem(keepId, keepName, missing: true);
            _cameraCombo.Items.Add(selected);
        }

        _cameraCombo.SelectedItem = selected; // null = nothing chosen (NO SIGNAL)
    }

    private void UpdateCameraPanel()
    {
        var item = _cameraCombo.SelectedItem as CameraItem;
        _cameraIdLabel.Text = item is null
            ? "Камера не выбрана — хост будет показывать NO SIGNAL."
            : item.Missing
                ? $"ID: {item.Id}  — устройство сейчас недоступно, выбор сохранён."
                : $"ID: {item.Id}";
    }

    // Device list status + quiet knob errors from the controls panel.
    private void UpdateCameraStatus(string? extra)
    {
        _cameraStatus.Text = string.IsNullOrEmpty(extra)
            ? _cameraListStatus
            : _cameraListStatus + "\r\n" + extra;
    }

    private void OnHelpClicked(object? sender, EventArgs e)
    {
        using var help = new HelpForm();
        help.ShowDialog(this);
    }

    // В1: асинхронно (см. LoadCameraListAsync) — форму не вешает.
    private async void OnCameraRefreshClicked(object? sender, EventArgs e)
    {
        await LoadCameraListAsync();
    }

    private void OnOpenClicked(object? sender, EventArgs e)
    {
        if (CurrentSourceType == SourceTypes.Video)
        {
            using var dlg = new OpenFileDialog
            {
                Title = "Выберите видеоролик",
                Filter = "Видео|*.mp4;*.mkv;*.avi;*.mov;*.wmv;*.webm;*.webp|Все файлы|*.*",
            };
            if (dlg.ShowDialog(this) != DialogResult.OK) return;
            _videoPath = dlg.FileName;
            UpdateLayout();
            MarkDirty();
            return;
        }

        using (var dlg = new OpenFileDialog
        {
            Title = "Выберите изображение",
            Filter = "Изображения|*.png;*.jpg;*.jpeg;*.jfif;*.bmp;*.gif;*.tif;*.tiff;*.webp|Все файлы|*.*",
        })
        {
            if (dlg.ShowDialog(this) != DialogResult.OK) return;
            SetSource(dlg.FileName, null);
        }
    }

    private void SetSource(string path, Rectangle? savedCrop)
    {
        try
        {
            // Load via stream so the file stays unlocked for other processes.
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var img = Image.FromStream(fs);
            var bmp = new Bitmap(img); // detach from the stream
            img.Dispose();

            _sourceImage?.Dispose();
            _sourceImage = bmp;
            _sourcePath = path;
            _pathLabel.Text = path;
            _fullSizeButton.Enabled = true;

            // Reset crop view so the new image (and its default/saved selection) wins.
            _cropView.Visible = false;
            var oldCropImage = _cropView.Image;
            _cropView.Image = null;
            if (oldCropImage != null && !ReferenceEquals(oldCropImage, bmp)) { /* Image is the source itself; do not dispose */ }

            if (CurrentMode == ScaleMode.Crop)
            {
                _cropView.Visible = true;
                _cropView.Image = bmp; // default selection
                if (savedCrop is { Width: > 0, Height: > 0 } &&
                    savedCrop.Value.Right <= bmp.Width && savedCrop.Value.Bottom <= bmp.Height)
                {
                    _cropView.Selection = savedCrop.Value;
                }
                UpdateCropFieldsFromSelection();
            }
            else
            {
                UpdatePreview();
            }
            MarkDirty(); // file pick; programmatic loads run suppressed
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось открыть изображение:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    // Встроенное превью — исходник как есть (только Fit/Cover): кадр эфира
    // совпадает с превью (пост-процессинга в хосте сейчас нет — только
    // no-op хук PostProcessFrame под будущую обработку).
    private void UpdatePreview()
    {
        if (_sourceImage is null) return;
        var old = _preview.Image;
        _preview.Image = PreviewRenderer.Render(_sourceImage, CurrentMode == ScaleMode.Cover ? ScaleMode.Cover : ScaleMode.Fit);
        old?.Dispose();
    }

    private void OnCropSelectionChanged()
    {
        UpdateCropFieldsFromSelection();
        UpdateCropPreviewImage();
        MarkDirty(); // rectangle drag; programmatic syncs run suppressed
    }

    private void UpdateCropPreviewImage()
    {
        // Result preview is implicit: the crop view itself shows the kept area;
        // final stretch is applied by StaticProducer. Keep fields in sync only.
    }

    private void UpdateCropFieldsFromSelection()
    {
        if (_sourceImage is null) return;
        var sel = _cropView.Selection;
        _updatingCropFields = true;
        try
        {
            _cropX.Value = Math.Clamp(sel.X, 0, (int)_cropX.Maximum);
            _cropY.Value = Math.Clamp(sel.Y, 0, (int)_cropY.Maximum);
            _cropW.Value = Math.Clamp(sel.Width, 0, (int)_cropW.Maximum);
            _cropH.Value = Math.Clamp(sel.Height, 0, (int)_cropH.Maximum);
        }
        finally { _updatingCropFields = false; }
    }

    private void OnCropFieldChanged(object? sender, EventArgs e)
    {
        if (_updatingCropFields || _sourceImage is null) return;
        MarkDirty(); // numeric edit (may clamp to the same rect: no Selection event)
        _updatingCropFields = true;
        try
        {
            var rect = PreviewRenderer.ClampCrop(_sourceImage,
                new Rectangle((int)_cropX.Value, (int)_cropY.Value, (int)_cropW.Value, (int)_cropH.Value));
            _cropView.Selection = rect;
        }
        finally { _updatingCropFields = false; }
    }

    private void OnFullSizeClicked(object? sender, EventArgs e)
    {
        if (_sourceImage is null) return;
        new FullSizeForm(_sourceImage, _sourcePath).Show(this);
    }

    private void OnPreviewClicked(object? sender, EventArgs e)
    {
        if (_previewExe is null) return;
        try
        {
            Process.Start(new ProcessStartInfo(_previewExe)
            {
                WorkingDirectory = Path.GetDirectoryName(_previewExe) ?? AppContext.BaseDirectory,
            });
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось запустить VCamPreview:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private void OnSaveClicked(object? sender, EventArgs e)
    {
        var built = CollectSettingsFromControls();
        if (built is null) return;
        TrySave(built.Value.Settings, built.Value.OkText, built.Value.Warn);
    }

    // Builds Settings from the controls (old OnSaveClicked body, unchanged
    // mapping): starts from what is on disk so autostart and the section
    // that is not being edited stay untouched; overwrites the edited
    // section + type + quality. Shared by "Сохранить настройки".
    // Null = validation failed (shown already).
    // К1: битый settings.json НЕ затираем дефолтами — читаем строгим
    // LoadFromText и отменяем Save при исключении.
    private (Settings Settings, string OkText, bool Warn)? CollectSettingsFromControls()
    {
        Settings settings;
        var liveText = ReadSettingsText();
        if (string.IsNullOrWhiteSpace(liveText))
        {
            settings = new Settings(); // файла ещё нет — первое сохранение
        }
        else
        {
            try
            {
                settings = Settings.LoadFromText(liveText);
            }
            catch (Exception ex)
            {
                MessageBox.Show(this, $"Файл настроек повреждён — сохранение отменено, чтобы не затереть его значениями по умолчанию:\n{ex.Message}", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Error);
                return null;
            }
        }
        settings.RecordPath = _recPathText.Text.Trim();
        settings.HotkeyModifiers = _hotkeyMods;
        settings.HotkeyVk = _hotkeyVk;
        settings.RecordHotkeyModifiers = _recHotkeyMods;
        settings.RecordHotkeyVk = _recHotkeyVk;
        settings.VideoHotkeyModifiers = _videoHotkeyMods;
        settings.VideoHotkeyVk = _videoHotkeyVk;
        // video.loop lives in the video panel but saves regardless of the
        // selected mode (the value round-trips from Load otherwise).
        settings.VideoLoop = _videoLoopCheck.Checked;

        if (CurrentSourceType == SourceTypes.Video)
        {
            if (string.IsNullOrEmpty(_videoPath))
            {
                MessageBox.Show(this, "Сначала выберите видеоролик.", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return null;
            }
            if (!File.Exists(_videoPath))
            {
                MessageBox.Show(this, "Файл ролика не найден — выберите его заново.", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return null;
            }

            settings.VideoPath = _videoPath;
            settings.SourceType = SourceTypes.Video;
            settings.Quality = CurrentQuality;
            return (settings,
                $"Сохранено: {Settings.FilePath} — хост подхватит source.type/video.path (~1 с).",
                false);
        }

        if (CurrentSourceType == SourceTypes.Camera)
        {
            var cam = _cameraCombo.SelectedItem as CameraItem;
            settings.CameraId = cam?.Id ?? "";
            settings.CameraName = cam?.Name ?? "";
            settings.Capture = CurrentCapture;
            settings.SourceType = SourceTypes.Camera;
            settings.Quality = CurrentQuality;
            if (cam is null)
            {
                return (settings,
                    $"Сохранено: {Settings.FilePath} — камера НЕ выбрана, хост будет показывать NO SIGNAL, пока вы не выберете устройство.",
                    true);
            }
            return (settings,
                $"Сохранено: {Settings.FilePath} — хост подхватит source.type/camera (~1 с): {cam.Name}.",
                false);
        }

        if (string.IsNullOrEmpty(_sourcePath))
        {
            MessageBox.Show(this, "Сначала выберите картинку.", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return null;
        }

        settings.StaticPath = _sourcePath;
        settings.ScaleMode = CurrentMode;
        if (CurrentMode == ScaleMode.Crop && _sourceImage != null)
        {
            var sel = PreviewRenderer.ClampCrop(_sourceImage, _cropView.Selection);
            settings.CropX = sel.X;
            settings.CropY = sel.Y;
            settings.CropW = sel.Width;
            settings.CropH = sel.Height;
            settings.CropKeepAspect = _cropKeepAspect.Checked;
        }
        // В3: комбо показывает static и для неизвестного будущего токена.
        // Пока пользователь явно не трогал выбор медиа — храним токен verbatim
        // (как C++), иначе любой Save схлопнул бы будущее в "static".
        // Явный выбор (в т.ч. возврат на «статичная картинка») пишет выбор.
        if (CurrentSourceType != SourceTypes.Static || _mediaChangedByUser ||
            SourceTypes.IsKnown(settings.SourceType))
            settings.SourceType = CurrentSourceType;
        settings.Quality = CurrentQuality;

        return (settings,
            $"Сохранено: {Settings.FilePath} — хост подхватит source.type/static (~1 с).",
            false);
    }

    private void TrySave(Settings settings, string okText, bool warn = false)
    {
        try
        {
            settings.Save();
            // What is on disk now matches the controls: clear the dirty flag
            // and the external-change indicator, remember the snapshot so the
            // watcher recognises its echo as our own write.
            _dirty = false;
            _syncRetries = 0;
            _reloadButton.Text = "Обновить";
            _lastAppliedText = ReadSettingsText();
            _hintLabel.ForeColor = warn ? Color.DarkGoldenrod : Color.ForestGreen;
            _hintLabel.Text = okText;
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось сохранить настройки:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    protected override void OnFormClosed(FormClosedEventArgs e)
    {
        _syncTimer.Stop();
        _syncTimer.Dispose();
        if (_settingsWatcher is not null)
        {
            _settingsWatcher.EnableRaisingEvents = false;
            _settingsWatcher.Dispose();
            _settingsWatcher = null;
        }
        _sourceImage?.Dispose();
        var img = _preview.Image;
        _preview.Image = null;
        img?.Dispose();
        base.OnFormClosed(e);
    }
}
