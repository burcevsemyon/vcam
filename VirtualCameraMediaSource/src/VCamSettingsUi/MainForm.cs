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

    // Host post-fx (phase vcam-effects + analog): mirror + grayscale +
    // analog interferences (noise/scanlines/rgbsplit/tracking/vhs),
    // section "effects". Two rows of checkboxes (y=600/622).
    private readonly CheckBox _fxMirror = new();
    private readonly CheckBox _fxGrayscale = new();
    private readonly CheckBox _fxNoise = new();
    private readonly CheckBox _fxScanlines = new();
    private readonly CheckBox _fxRgbSplit = new();
    private readonly CheckBox _fxTracking = new();
    private readonly CheckBox _fxVhs = new();

    // Analog interference intensity sliders (TrackBar 0-100) next to the
    // interference checkboxes; section "effects" levels. VHS has no own
    // slider: it uses these four individual levels.
    private readonly TrackBar _fxNoiseLevel = new();
    private readonly TrackBar _fxScanlinesLevel = new();
    private readonly TrackBar _fxRgbSplitLevel = new();
    private readonly TrackBar _fxTrackingLevel = new();
    private readonly Label _fxNoiseLevelVal = new();
    private readonly Label _fxScanlinesLevelVal = new();
    private readonly Label _fxRgbSplitLevelVal = new();
    private readonly Label _fxTrackingLevelVal = new();

    // Analog interference backend (ComboBox in the effects group header):
    // CPU (default, no DLLs) | frei0r (frei0r plugin chain, falls back to
    // CPU when the DLLs are missing).
    private readonly ComboBox _fxBackend = new();
    private readonly Label _fxBackendLabel = new();

    // Effects master switch (header row of the effects group): false = the
    // host skips ApplyFx entirely. Unchecked grays out the rows below.
    private readonly CheckBox _fxEnabled = new();

    // Settings profiles (header row of the effects group): named FULL snapshots
    // stored as plain Settings files in %APPDATA%\VCam\profiles\. "Сохранить…"
    // writes the current settings as-is under a name; choosing the ComboBox
    // (or "Применить") replaces settings.json with the profile file
    // byte-for-byte — source, effects, everything — so a source switch
    // re-opens the source (~1 s, normal and predictable). The host picks the
    // change up via hot-reload as usual. Choosing the ComboBox applies
    // immediately (explicit user action, dirty is reset); "Применить"
    // re-applies the same profile.
    // Programmatic selection (RefreshProfileList/startup) runs under
    // _refreshingProfiles and never applies.
    private readonly Label _profileLabel = new();
    private readonly ComboBox _profileCombo = new();
    private readonly Button _profileApply = new();
    private readonly Button _profileSave = new();
    private readonly Button _profileDelete = new();
    private bool _refreshingProfiles; // programmatic combo set, not a choice

    // Effects section container: GroupBox "Эффекты" with a header row (backend
    // switch) and a TableLayoutPanel (row = checkbox + slider + value).
    // Table layout (AutoSize checkbox/value columns) keeps all 7 checkboxes +
    // 4 sliders + backend visible without overlaps at 100% and 125% DPI —
    // fixed X positions used to overlap once the font scaled up.
    private readonly GroupBox _fxGroup = new();
    private readonly Panel _fxHeader = new();
    private readonly TableLayoutPanel _fxTable = new();

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
    // The combination itself lives in settings.json (hotkey {modifiers, vk});
    // there is no editor — a wrong value falls back to Ctrl+Alt+V in host+UI.
    private readonly Label _hotkeyHint = new();

    // Ether recording to .mp4 (host SinkWriter, frames with effects applied):
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
    private string _lastAppliedText = ""; // file snapshot the controls reflect
    private int _syncRetries;

    private static readonly string[] ModeNames = { "fit — вписать с пололосами", "cover — заполнить (обрезка)", "crop — обрезка выбранной области" };
    private static readonly string[] MediaNames = { "статичная картинка", "видеоролик", "физическая камера" };
    private static readonly string[] QualityNames = { "натив (source)", "720p (fixed)" };
    private static readonly string[] CaptureNames = { "Максимум", "720p", "1080p" };
    private static readonly string[] FxBackendNames = { "CPU", "frei0r" };

    public MainForm()
    {
        Text = "VCam — настройки трансляции";
        // Sizable (was FixedDialog): the effects table needs the extra height,
        // and users on 125%+ DPI can grow the window instead of clipping.
        FormBorderStyle = FormBorderStyle.Sizable;
        MaximizeBox = true;
        MinimizeBox = true;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(880, 1082);
        MinimumSize = new Size(900, 1132);
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

        SetupVideoPanel();
        SetupCameraPanel();

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

        // Ether recording group (below the effects group): file path +
        // start/stop + REC indicator + record-hotkey hint.
        SetupRecGroup();

        // Manual reload from settings.json (always available; also the way out
        // when the file changed externally while the form is dirty).
        _reloadButton.Location = new Point(566, 1032);
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
        _hotkeyHint.Size = new Size(856, 22);
        _hotkeyHint.ForeColor = Color.DimGray;
        _hotkeyHint.Name = "hotkeyHint";
        _hotkeyHint.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

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

        // Effects section (always visible, below the crop fields row):
        // GroupBox "Эффекты" with a header (backend switch) and a table
        // (row = checkbox + slider + value). CheckBoxes/values are AutoSize,
        // sliders stretch (Dock Fill) — nothing overlaps at 100%/125% DPI.
        SetupFxGroup();

        _hintLabel.Location = new Point(12, 1034);
        _hintLabel.Size = new Size(548, 38);
        _hintLabel.ForeColor = Color.DimGray;
        _hintLabel.Text = $"Настройки: {Settings.FilePath} — хост VCam подхватит их автоматически (~1 с).";
        _hintLabel.Name = "hintLabel";
        _hintLabel.Anchor = AnchorStyles.Left | AnchorStyles.Bottom;

        _helpButton.Location = new Point(688, 1032);
        _helpButton.Size = new Size(180, 40);
        _helpButton.Text = "Справка…";
        _helpButton.Name = "helpButton";
        _helpButton.Click += OnHelpClicked;
        _helpButton.Anchor = AnchorStyles.Right | AnchorStyles.Bottom;

        Controls.AddRange(new Control[] { _preview, _cropView, _videoPanel, _cameraPanel, _pathLabel, _mediaLabel, _mediaCombo,
            _qualityLabel, _qualityCombo, _fxGroup, _recGroup, _hotkeyHint,
            _mode, _openButton, _fullSizeButton, _saveButton, _reloadButton, _hostStatusLabel, _hostButton, _helpButton,
            _cropXLabel, _cropX, _cropYLabel, _cropY, _cropWLabel, _cropW, _cropHLabel, _cropH, _cropKeepAspect, _hintLabel });

        _previewExe = FindPreviewExe();
        _hostExe = FindHostExe();
        _hostTimer.Interval = 1000;
        _hostTimer.Tick += (_, _) => UpdateHostStatus();
        _hostTimer.Start();
        UpdateHostStatus();

        _mode.SelectedIndexChanged += (_, _) => UpdateLayout();
        _mediaCombo.SelectedIndexChanged += (_, _) => UpdateLayout();
        _cameraCombo.SelectedIndexChanged += (_, _) => UpdateCameraPanel();
        _cameraRefresh.Click += OnCameraRefreshClicked;
        _controlsPanel.StatusMessage += UpdateCameraStatus;

        SubscribeDirtyTracking();
        LoadCurrentSettings();
        RefreshProfileList();
        UpdateFxEnabledState();
        UpdateLayout();
        InitSettingsSync();
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
        _videoInfoLabel.Size = new Size(820, 78);
        _videoInfoLabel.ForeColor = Color.DimGray;
        _videoInfoLabel.Text =
            "Ролик декодируется хостом VCamVideoStreamProducer.exe и всегда масштабируется letterbox в 1280×720.\r\n" +
            "Настройки scaleMode и crop для видео не применяются (см. секцию static).\r\n" +
            "Смена файла подхватывается автоматически (~1 с), без перезапуска.\r\n" +
            "Обычное видео крутится по кругу; ролик, вызванный горячей клавишей, " +
            "играет один раз и возвращает предыдущий источник.";

        _previewButton.Location = new Point(16, 176);
        _previewButton.Size = new Size(380, 40);
        _previewButton.Text = "Открыть окно предпросмотра (VCamPreview)";
        _previewButton.Name = "previewButton";
        _previewButton.Click += OnPreviewClicked;

        _previewHint.Location = new Point(16, 228);
        _previewHint.Size = new Size(820, 96);
        _previewHint.ForeColor = Color.DimGray;
        _previewHint.Name = "previewHint";

        // Borrowed-video indicator: empty unless the host holds a hotkey borrow
        // (transient hotkey_state.json). Free space below the preview hint.
        _hotkeyBorrowLabel.Location = new Point(16, 330);
        _hotkeyBorrowLabel.Size = new Size(820, 110);
        _hotkeyBorrowLabel.ForeColor = Color.DimGray;
        _hotkeyBorrowLabel.Name = "hotkeyBorrowLabel";

        _videoPanel.Controls.AddRange(new Control[] { _videoTitle, _videoPathLabel, _videoInfoLabel, _previewButton, _previewHint, _hotkeyBorrowLabel });
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

    // Effects GroupBox + table: header (backend switch) on top, 7 rows
    // (checkbox + slider + value) below. All checkbox/value cells are AutoSize
    // so longer labels at 125% DPI widen their column instead of overlapping
    // the neighbour (the old fixed-X layout clipped/overlapped).
    private void SetupFxGroup()
    {
        _fxGroup.Location = new Point(12, 614);
        _fxGroup.Size = new Size(856, 298);
        _fxGroup.Text = "Эффекты";
        _fxGroup.Name = "fxGroup";
        _fxGroup.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        _fxHeader.Dock = DockStyle.Top;
        _fxHeader.Height = 70;
        _fxHeader.Name = "fxHeader";

        // Row 1: profile picker (label + combo + apply/save/delete). The
        // combo stretches; the buttons keep fixed widths on the right.
        _profileLabel.AutoSize = true;
        _profileLabel.Location = new Point(10, 9);
        _profileLabel.Text = "Профиль:";
        _profileLabel.Name = "profileLabel";

        _profileCombo.Location = new Point(80, 5);
        _profileCombo.Size = new Size(430, 28);
        _profileCombo.DropDownStyle = ComboBoxStyle.DropDownList;
        _profileCombo.Name = "profileCombo";
        _profileCombo.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;
        // User choice applies immediately (same path as "Применить");
        // programmatic sets under _refreshingProfiles are ignored.
        _profileCombo.SelectedIndexChanged += OnProfileComboChanged;

        _profileApply.Location = new Point(516, 4);
        _profileApply.Size = new Size(100, 30);
        _profileApply.Text = "Применить";
        _profileApply.Name = "profileApply";
        _profileApply.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        _profileApply.Click += OnProfileApplyClicked;

        _profileSave.Location = new Point(622, 4);
        _profileSave.Size = new Size(118, 30);
        _profileSave.Text = "Сохранить…";
        _profileSave.Name = "profileSave";
        _profileSave.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        _profileSave.Click += OnProfileSaveClicked;

        _profileDelete.Location = new Point(746, 4);
        _profileDelete.Size = new Size(96, 30);
        _profileDelete.Text = "Удалить";
        _profileDelete.Name = "profileDelete";
        _profileDelete.Anchor = AnchorStyles.Top | AnchorStyles.Right;
        _profileDelete.Click += OnProfileDeleteClicked;

        // Row 2: master switch + backend. Everything below grays out while
        // the master is off (see UpdateFxEnabledState).
        _fxEnabled.AutoSize = true;
        _fxEnabled.Location = new Point(10, 41);
        _fxEnabled.Text = "Эффекты включены";
        _fxEnabled.Checked = true;
        _fxEnabled.Name = "fxEnabled";

        _fxBackendLabel.AutoSize = true;
        _fxBackendLabel.Location = new Point(220, 43);
        _fxBackendLabel.Text = "Backend:";
        _fxBackendLabel.Name = "fxBackendLabel";

        _fxBackend.Location = new Point(290, 39);
        _fxBackend.Size = new Size(110, 28);
        _fxBackend.DropDownStyle = ComboBoxStyle.DropDownList;
        _fxBackend.Items.AddRange(FxBackendNames);
        _fxBackend.SelectedIndex = 0;
        _fxBackend.Name = "fxBackend";
        _fxBackend.Anchor = AnchorStyles.Top | AnchorStyles.Left;
        _fxHeader.Controls.AddRange(new Control[] { _profileLabel, _profileCombo,
            _profileApply, _profileSave, _profileDelete,
            _fxEnabled, _fxBackendLabel, _fxBackend });

        _fxTable.Dock = DockStyle.Fill;
        _fxTable.Name = "fxTable";
        _fxTable.ColumnCount = 3;
        _fxTable.RowCount = 7;
        _fxTable.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        _fxTable.ColumnStyles.Add(new ColumnStyle(SizeType.Percent, 100f));
        _fxTable.ColumnStyles.Add(new ColumnStyle(SizeType.AutoSize));
        for (var r = 0; r < 7; r++)
            _fxTable.RowStyles.Add(new RowStyle(SizeType.Percent, 100f / 7f));

        SetupFxCheck(_fxMirror, "Зеркало", "fxMirror");
        SetupFxCheck(_fxGrayscale, "Ч/Б", "fxGrayscale");
        SetupFxCheck(_fxNoise, "Шум", "fxNoise");
        SetupFxCheck(_fxScanlines, "Сканлайны", "fxScanlines");
        SetupFxCheck(_fxRgbSplit, "RGB-сдвиг", "fxRgbSplit");
        SetupFxCheck(_fxTracking, "Трекинг", "fxTracking");
        SetupFxCheck(_fxVhs, "VHS (пресет)", "fxVhs");

        SetupFxLevel(_fxNoiseLevel, _fxNoiseLevelVal, "fxNoiseLevel");
        SetupFxLevel(_fxScanlinesLevel, _fxScanlinesLevelVal, "fxScanlinesLevel");
        SetupFxLevel(_fxRgbSplitLevel, _fxRgbSplitLevelVal, "fxRgbSplitLevel");
        SetupFxLevel(_fxTrackingLevel, _fxTrackingLevelVal, "fxTrackingLevel");

        // Rows: mirror / grayscale / noise / scanlines / rgbsplit / tracking / vhs.
        AddFxRow(0, _fxMirror, null, null);
        AddFxRow(1, _fxGrayscale, null, null);
        AddFxRow(2, _fxNoise, _fxNoiseLevel, _fxNoiseLevelVal);
        AddFxRow(3, _fxScanlines, _fxScanlinesLevel, _fxScanlinesLevelVal);
        AddFxRow(4, _fxRgbSplit, _fxRgbSplitLevel, _fxRgbSplitLevelVal);
        AddFxRow(5, _fxTracking, _fxTrackingLevel, _fxTrackingLevelVal);
        AddFxRow(6, _fxVhs, null, null);

        _fxGroup.Controls.Add(_fxTable);
        _fxGroup.Controls.Add(_fxHeader);
    }

    // Master switch gray-out: while the effects are disabled the rows below
    // (and the backend picker) are read-only. Only sets Enabled (no Checked
    // changes), so it never dirties the form and is safe under _suppressDirty.
    private void UpdateFxEnabledState()
    {
        var on = _fxEnabled.Checked;
        foreach (var c in new Control[] { _fxMirror, _fxGrayscale, _fxNoise,
            _fxScanlines, _fxRgbSplit, _fxTracking, _fxVhs,
            _fxNoiseLevel, _fxScanlinesLevel, _fxRgbSplitLevel, _fxTrackingLevel,
            _fxBackend })
            c.Enabled = on;
    }

    // Ether recording group: file path + browse + start/stop + REC line +
    // record-hotkey hint. Below the effects group (fx bottom = 912).
    private void SetupRecGroup()
    {
        _recGroup.Location = new Point(12, 916);
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
        _recStatus.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        _recHint.Location = new Point(16, 79);
        _recHint.Size = new Size(824, 22);
        _recHint.ForeColor = Color.DimGray;
        _recHint.Name = "recHint";
        _recHint.Anchor = AnchorStyles.Top | AnchorStyles.Left | AnchorStyles.Right;

        _recGroup.Controls.AddRange(new Control[] {
            _recPathLabel, _recPathText, _recBrowse, _recButton, _recStatus, _recHint });
    }

    private static void SetupFxCheck(CheckBox box, string text, string name)
    {
        box.AutoSize = true;
        box.Text = text;
        box.Name = name;
        box.Anchor = AnchorStyles.Left;
        box.Margin = new Padding(6, 3, 6, 3);
    }

    private void AddFxRow(int row, CheckBox box, TrackBar? bar, Label? val)
    {
        box.Dock = DockStyle.Fill;
        _fxTable.Controls.Add(box, 0, row);
        if (bar is not null && val is not null)
        {
            bar.Dock = DockStyle.Fill;
            _fxTable.Controls.Add(bar, 1, row);
            val.Dock = DockStyle.Fill;
            _fxTable.Controls.Add(val, 2, row);
        }
    }

    // Intensity slider 0-100 next to an interference checkbox: compact
    // TrackBar (no ticks) + numeric value label. Scroll updates the label.
    // Geometry is owned by the effects table (Dock Fill); no coordinates here.
    private void SetupFxLevel(TrackBar bar, Label val, string name)
    {
        bar.Minimum = 0;
        bar.Maximum = 100;
        bar.TickStyle = TickStyle.None;
        bar.SmallChange = 5;
        bar.LargeChange = 10;
        bar.Value = 100;
        bar.Name = name;
        bar.Margin = new Padding(6, 0, 6, 0);
        val.AutoSize = true;
        val.MinimumSize = new Size(30, 0);
        val.Text = "100";
        val.TextAlign = ContentAlignment.MiddleLeft;
        val.Name = name + "Val";
        val.Margin = new Padding(0, 3, 6, 3);
        bar.Scroll += (_, _) => val.Text = bar.Value.ToString();
        // ValueChanged covers Scroll + keyboard + programmatic sets; the label
        // stays in sync and user edits set the dirty flag (programmatic sets
        // during Load/reload are suppressed via _suppressDirty).
        bar.ValueChanged += (_, _) => { val.Text = bar.Value.ToString(); MarkDirty(); };
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
        foreach (var c in new[] { _fxMirror, _fxGrayscale, _fxNoise, _fxScanlines,
                                  _fxRgbSplit, _fxTracking, _fxVhs, _cropKeepAspect })
            c.CheckedChanged += (_, _) => MarkDirty();
        // Master switch: user edits set the dirty flag (suppressed during
        // Load/reload); the gray-out below always follows the checkbox.
        _fxEnabled.CheckedChanged += (_, _) => { UpdateFxEnabledState(); MarkDirty(); };
        // TrackBars are covered in SetupFxLevel (ValueChanged).
        _mode.SelectedIndexChanged += (_, _) => MarkDirty();
        _mediaCombo.SelectedIndexChanged += (_, _) => MarkDirty();
        _qualityCombo.SelectedIndexChanged += (_, _) => MarkDirty();
        _fxBackend.SelectedIndexChanged += (_, _) => MarkDirty();
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

    private void OnHostButtonClicked(object? sender, EventArgs e)
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
            _hostStatusLabel.Text = "Хост: перезапускается…";
            var deadline = Environment.TickCount64 + 5000;
            while (IsHostRunning() && Environment.TickCount64 < deadline)
                Thread.Sleep(100);
            if (IsHostRunning())
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
        _cropKeepAspect.Checked = s.CropKeepAspect;
        UpdateHotkeyHint(s);
        UpdateHotkeyBorrowLabel();
        _recPathText.Text = string.IsNullOrEmpty(s.RecordPath) ? DefaultRecPath() : s.RecordPath;
        UpdateRecHint(s);
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
        _qualityCombo.SelectedIndex = s.Quality == Quality.Fixed720p ? 1 : 0;
        _fxEnabled.Checked = s.FxEnabled;
        _fxMirror.Checked = s.FxMirror;
        _fxGrayscale.Checked = s.FxGrayscale;
        _fxNoise.Checked = s.FxNoise;
        _fxScanlines.Checked = s.FxScanlines;
        _fxRgbSplit.Checked = s.FxRgbSplit;
        _fxTracking.Checked = s.FxTracking;
        _fxVhs.Checked = s.FxVhs;
        _fxNoiseLevel.Value = Math.Clamp(s.FxNoiseLevel, 0, 100);
        _fxNoiseLevelVal.Text = _fxNoiseLevel.Value.ToString();
        _fxScanlinesLevel.Value = Math.Clamp(s.FxScanlinesLevel, 0, 100);
        _fxScanlinesLevelVal.Text = _fxScanlinesLevel.Value.ToString();
        _fxRgbSplitLevel.Value = Math.Clamp(s.FxRgbSplitLevel, 0, 100);
        _fxRgbSplitLevelVal.Text = _fxRgbSplitLevel.Value.ToString();
        _fxTrackingLevel.Value = Math.Clamp(s.FxTrackingLevel, 0, 100);
        _fxTrackingLevelVal.Text = _fxTrackingLevel.Value.ToString();
        _fxBackend.SelectedIndex = s.FxBackend == "frei0r" ? 1 : 0;
        _mediaCombo.SelectedIndex = s.SourceType switch
        {
            SourceType.Video => 1,
            SourceType.Camera => 2,
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
            // Missing file (or an applied profile pointing at one): remember
            // the path anyway so a later save writes exactly what is on disk,
            // and drop the stale picture so the preview never lies.
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

    private SourceType CurrentSourceType => _mediaCombo.SelectedIndex switch
    {
        1 => SourceType.Video,
        2 => SourceType.Camera,
        _ => SourceType.Static,
    };

    private Quality CurrentQuality => _qualityCombo.SelectedIndex == 1 ? Quality.Fixed720p : Quality.Source;

    // Backend ComboBox: index 1 = frei0r, anything else = cpu.
    private string CurrentBackend => _fxBackend.SelectedIndex == 1 ? "frei0r" : "cpu";

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
        bool video = CurrentSourceType == SourceType.Video;
        bool camera = CurrentSourceType == SourceType.Camera;
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
    private static string HotkeyDisplay(Settings s) =>
        HotkeyDisplayMods(s.HotkeyModifiers, s.HotkeyVk);

    // Same for the record hotkey (settings recordHotkey, default Ctrl+Alt+R).
    private static string RecordHotkeyDisplay(Settings s) =>
        HotkeyDisplayMods(s.RecordHotkeyModifiers, s.RecordHotkeyVk);

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

    // Always-visible hotkey line: current combination + what it does.
    private void UpdateHotkeyHint(Settings s)
    {
        _hotkeyHint.Text = "Горячая клавиша: " + HotkeyDisplay(s) +
            " — показать видео один раз (повторно — вернуться сразу; " +
            "после конца ролика — автовозврат). Комбинация — в settings.json (hotkey).";
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

    private void UpdateRecHint(Settings s)
    {
        _recHint.Text = "Горячая клавиша записи: " + RecordHotkeyDisplay(s) +
            " — старт/стоп (комбинация — в settings.json (recordHotkey)).";
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
        var path = _recPathText.Text.Trim();
        if (string.IsNullOrEmpty(path))
        {
            path = DefaultRecPath();
            _recPathText.Text = path;
        }
        // Лёгкий Save только record.path поверх диска (без валидации
        // source-секций из CollectSettingsFromControls — старт записи не
        // должен упираться в незаполненный источник).
        try
        {
            var s = Settings.Load();
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
    private void EnsureCameraList()
    {
        if (_cameraListLoaded) return;
        LoadCameraList();
    }

    // "VCamProducerCli list-devices": stdout = rows "<id>\t<name>" (UTF-8),
    // the header goes to stderr and is ignored by the parser. Exit code is
    // always 0 (including zero devices).
    private void LoadCameraList()
    {
        _cameraListLoaded = true;
        _cameraItems.Clear();
        string status;

        _cliExe ??= FindCliExe();
        if (_cliExe is null)
        {
            status = "VCamProducerCli.exe не найден: искал рядом с VCamSettingsUi.exe и в " +
                     "<корень репозитория>\\build\\x64\\Release — список камер недоступен, выберите «Обновить список» после установки.";
        }
        else
        {
            try
            {
                using var proc = Process.Start(new ProcessStartInfo(_cliExe)
                {
                    Arguments = "list-devices",
                    UseShellExecute = false,
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8,
                    CreateNoWindow = true,
                    WorkingDirectory = Path.GetDirectoryName(_cliExe) ?? AppContext.BaseDirectory,
                });
                if (proc is null)
                {
                    status = "Не удалось запустить VCamProducerCli — список камер недоступен.";
                }
                else
                {
                    // Drain both pipes on background tasks so a full stderr buffer
                    // cannot deadlock the wait below.
                    var outTask = proc.StandardOutput.ReadToEndAsync();
                    var errTask = proc.StandardError.ReadToEndAsync();
                    var exited = proc.WaitForExit(8000);
                    if (!exited)
                    {
                        try { proc.Kill(); } catch { /* already gone */ }
                        status = "VCamProducerCli list-devices не завершился за 8 с — список не получен.";
                    }
                    else
                    {
                        ParseListDevices(outTask.Result);
                        var err = errTask.Result.Trim();
                        status = _cameraItems.Count > 0
                            ? $"Камер найдено: {_cameraItems.Count}."
                            : "Камеры не найдены (0)." + (err.Length > 0 ? $" {err}" : "");
                    }
                }
            }
            catch (Exception ex)
            {
                status = $"Не удалось получить список камер: {ex.Message}";
            }
        }

        FillCameraCombo();
        _cameraListStatus = status;
        UpdateCameraStatus(null);
        UpdateCameraPanel();
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

    private void OnCameraRefreshClicked(object? sender, EventArgs e)
    {
        _cameraRefresh.Enabled = false;
        try
        {
            LoadCameraList();
        }
        finally
        {
            _cameraRefresh.Enabled = true;
        }
    }

    private void OnOpenClicked(object? sender, EventArgs e)
    {
        if (CurrentSourceType == SourceType.Video)
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
    // section + type + effects + quality. Shared by "Сохранить настройки"
    // and "Сохранить…" (profile). Null = validation failed (shown already).
    private (Settings Settings, string OkText, bool Warn)? CollectSettingsFromControls()
    {
        var settings = Settings.Load();
        settings.RecordPath = _recPathText.Text.Trim();
        settings.FxEnabled = _fxEnabled.Checked;
        settings.FxMirror = _fxMirror.Checked;
        settings.FxGrayscale = _fxGrayscale.Checked;
        settings.FxNoise = _fxNoise.Checked;
        settings.FxScanlines = _fxScanlines.Checked;
        settings.FxRgbSplit = _fxRgbSplit.Checked;
        settings.FxTracking = _fxTracking.Checked;
        settings.FxVhs = _fxVhs.Checked;
        settings.FxNoiseLevel = _fxNoiseLevel.Value;
        settings.FxScanlinesLevel = _fxScanlinesLevel.Value;
        settings.FxRgbSplitLevel = _fxRgbSplitLevel.Value;
        settings.FxTrackingLevel = _fxTrackingLevel.Value;
        settings.FxBackend = CurrentBackend;

        if (CurrentSourceType == SourceType.Video)
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
            settings.SourceType = SourceType.Video;
            settings.Quality = CurrentQuality;
            return (settings,
                $"Сохранено: {Settings.FilePath} — хост подхватит source.type/video.path (~1 с).",
                false);
        }

        if (CurrentSourceType == SourceType.Camera)
        {
            var cam = _cameraCombo.SelectedItem as CameraItem;
            settings.CameraId = cam?.Id ?? "";
            settings.CameraName = cam?.Name ?? "";
            settings.Capture = CurrentCapture;
            settings.SourceType = SourceType.Camera;
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
        settings.SourceType = SourceType.Static;
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

    // Rebuilds the profile ComboBox (seeded on first launch). Keeps the
    // requested selection when possible, otherwise the previous one. Never
    // writes settings.json, never dirties the form.
    private void RefreshProfileList(string? select = null)
    {
        string? keep = select ?? _profileCombo.SelectedItem as string;
        List<string> names;
        try
        {
            Profiles.EnsureSeeded();
            names = Profiles.List();
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"MainForm: profiles unavailable: {ex.Message}");
            return;
        }

        var prev = _suppressDirty;
        _suppressDirty = true;
        _refreshingProfiles = true;
        try
        {
            _profileCombo.Items.Clear();
            foreach (var n in names)
                _profileCombo.Items.Add(n);
            if (keep is not null && names.Contains(keep, StringComparer.Ordinal))
                _profileCombo.SelectedItem = keep;
            else if (names.Count > 0)
                _profileCombo.SelectedIndex = 0;
        }
        finally
        {
            _refreshingProfiles = false;
            _suppressDirty = prev;
        }
    }

    // User picked a profile in the ComboBox: apply immediately (explicit
    // action — applies even when the form is dirty, dirty is reset).
    // Programmatic sets (RefreshProfileList/startup) are flagged and ignored.
    private void OnProfileComboChanged(object? sender, EventArgs e)
    {
        if (_refreshingProfiles || _suppressDirty) return;
        var name = _profileCombo.SelectedItem as string;
        if (string.IsNullOrEmpty(name)) return;
        ApplyProfile(name);
    }

    // Apply = FULL snapshot: the profile file's bytes (validated by parsing
    // first) replace settings.json entirely — source, effects, everything.
    // The host picks the change up via hot-reload; a source switch re-opens
    // the source (~1 s — normal and predictable). A corrupt profile aborts
    // with a message and never touches the live settings.
    private void OnProfileApplyClicked(object? sender, EventArgs e)
    {
        var name = _profileCombo.SelectedItem as string;
        if (string.IsNullOrEmpty(name))
        {
            MessageBox.Show(this, "Нет профилей — сохраните текущий кнопкой «Сохранить…».", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }
        ApplyProfile(name);
    }

    private void ApplyProfile(string name)
    {
        string profilePath;
        string text;
        try
        {
            profilePath = Profiles.PathFor(name);
            text = File.ReadAllText(profilePath);
            // Validate before writing: a broken profile must never land in
            // the live settings (LoadFromText throws on malformed JSON).
            _ = Settings.LoadFromText(text);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось прочитать профиль «{name}»:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        try
        {
            // Byte-for-byte: the live file becomes the profile file exactly.
            File.Copy(profilePath, Settings.FilePath, overwrite: true);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось применить профиль «{name}»:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        // Full control refresh (same mapping as startup/reload, source panels
        // included — the profile may have switched source/camera/video).
        // The validated text is applied directly (no TOCTOU re-read); the
        // watcher recognises the copy as our own write via _lastAppliedText.
        ApplySettingsText(text);
        UpdateFxEnabledState();

        _hintLabel.ForeColor = Color.ForestGreen;
        _hintLabel.Text = $"Профиль «{name}» применён целиком — все настройки заменены из профиля (включая источник), хост подхватит (~1 с).";
    }

    private void OnProfileSaveClicked(object? sender, EventArgs e)
    {
        var current = _profileCombo.SelectedItem as string ?? "";
        var name = PromptProfileName(current);
        if (name is null) return; // cancelled

        var built = CollectSettingsFromControls();
        if (built is null) return;

        if (Profiles.Exists(name) &&
            MessageBox.Show(this, $"Профиль «{name}» уже есть. Перезаписать?", Text,
                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return;

        try
        {
            Profiles.SaveProfile(name, built.Value.Settings);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось сохранить профиль «{name}»:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        RefreshProfileList(name);
        _hintLabel.ForeColor = Color.ForestGreen;
        _hintLabel.Text = $"Профиль «{name}» сохранён (полный снимок всех настроек). Выбор в списке заменяет все настройки целиком.";
    }

    private void OnProfileDeleteClicked(object? sender, EventArgs e)
    {
        var name = _profileCombo.SelectedItem as string;
        if (string.IsNullOrEmpty(name)) return;
        if (MessageBox.Show(this, $"Удалить профиль «{name}»?", Text,
                MessageBoxButtons.YesNo, MessageBoxIcon.Question) != DialogResult.Yes)
            return;

        try
        {
            Profiles.DeleteProfile(name);
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось удалить профиль «{name}»:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
            return;
        }

        RefreshProfileList();
        _hintLabel.ForeColor = Color.DimGray;
        _hintLabel.Text = $"Профиль «{name}» удалён.";
    }

    // Minimal name prompt (WinForms has no built-in input box): modal dialog
    // with a TextBox, OK/Cancel. Null = cancelled.
    private string? PromptProfileName(string initial)
    {
        using var dlg = new Form
        {
            Text = "Сохранить профиль",
            FormBorderStyle = FormBorderStyle.FixedDialog,
            StartPosition = FormStartPosition.CenterParent,
            ClientSize = new Size(360, 110),
            MaximizeBox = false,
            MinimizeBox = false,
            ShowInTaskbar = false,
        };
        var box = new TextBox
        {
            Location = new Point(12, 12),
            Size = new Size(336, 28),
            Text = initial,
            MaxLength = 80,
        };
        box.SelectAll();
        var ok = new Button
        {
            Location = new Point(192, 56),
            Size = new Size(75, 32),
            Text = "OK",
            DialogResult = DialogResult.OK,
        };
        var cancel = new Button
        {
            Location = new Point(273, 56),
            Size = new Size(75, 32),
            Text = "Отмена",
            DialogResult = DialogResult.Cancel,
        };
        dlg.Controls.AddRange(new Control[] { box, ok, cancel });
        dlg.AcceptButton = ok;
        dlg.CancelButton = cancel;
        if (dlg.ShowDialog(this) != DialogResult.OK) return null;
        var clean = Profiles.Sanitize(box.Text);
        return string.IsNullOrEmpty(clean) ? null : clean;
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
