using System.Diagnostics;
using System.Text;

namespace VCamSettingsUi;

public sealed class MainForm : Form
{
    private readonly PictureBox _preview = new();
    private readonly CropPreviewControl _cropView = new();
    private readonly ComboBox _mode = new();
    private readonly Button _openButton = new();
    private readonly Button _fullSizeButton = new();
    private readonly Button _saveButton = new();
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

    // Host (VCamVideoStreamProducer.exe): start/stop button + status indicator.
    private readonly Button _hostButton = new();
    private readonly Label _hostStatusLabel = new();
    private readonly System.Windows.Forms.Timer _hostTimer = new();
    private readonly Button _helpButton = new();
    private string? _hostExe;

    private const string HostMutexName = "VCamVideoStreamProducer.Instance";
    private const string HostStopEventName = "VCamVideoStreamProducer.Stop";

    private static readonly string[] ModeNames = { "fit — вписать с пололосами", "cover — заполнить (обрезка)", "crop — обрезка выбранной области" };
    private static readonly string[] MediaNames = { "статичная картинка", "видеоролик", "физическая камера" };
    private static readonly string[] QualityNames = { "натив (source)", "720p (fixed)" };
    private static readonly string[] CaptureNames = { "Максимум", "720p", "1080p" };

    public MainForm()
    {
        Text = "VCam — настройки трансляции";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(880, 656);
        Font = new Font("Segoe UI", 9f);

        _preview.Location = new Point(12, 12);
        _preview.Size = new Size(856, 455); // preview box; Zoom letterboxes 16:9 on black
        _preview.SizeMode = PictureBoxSizeMode.Zoom;
        _preview.BackColor = Color.Black;
        _preview.BorderStyle = BorderStyle.FixedSingle;
        _preview.Name = "preview";

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

        _hostStatusLabel.Location = new Point(430, 538);
        _hostStatusLabel.Size = new Size(244, 22);
        _hostStatusLabel.ForeColor = Color.DimGray;
        _hostStatusLabel.Name = "hostStatusLabel";

        _hostButton.Location = new Point(680, 532);
        _hostButton.Size = new Size(188, 30);
        _hostButton.Name = "hostButton";
        _hostButton.Click += OnHostButtonClicked;

        int fieldY = 566;
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

        _hintLabel.Location = new Point(12, 602);
        _hintLabel.Size = new Size(660, 50);
        _hintLabel.ForeColor = Color.DimGray;
        _hintLabel.Text = $"Настройки: {Settings.FilePath} — хост VCam подхватит их автоматически (~1 с).";
        _hintLabel.Name = "hintLabel";

        _helpButton.Location = new Point(688, 604);
        _helpButton.Size = new Size(180, 40);
        _helpButton.Text = "Справка…";
        _helpButton.Name = "helpButton";
        _helpButton.Click += OnHelpClicked;

        Controls.AddRange(new Control[] { _preview, _cropView, _videoPanel, _cameraPanel, _pathLabel, _mediaLabel, _mediaCombo,
            _qualityLabel, _qualityCombo,
            _mode, _openButton, _fullSizeButton, _saveButton, _hostStatusLabel, _hostButton, _helpButton,
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

        LoadCurrentSettings();
        UpdateLayout();
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
            "Смена файла подхватывается автоматически (~1 с), без перезапуска.";

        _previewButton.Location = new Point(16, 176);
        _previewButton.Size = new Size(380, 40);
        _previewButton.Text = "Открыть окно предпросмотра (VCamPreview)";
        _previewButton.Name = "previewButton";
        _previewButton.Click += OnPreviewClicked;

        _previewHint.Location = new Point(16, 228);
        _previewHint.Size = new Size(820, 96);
        _previewHint.ForeColor = Color.DimGray;
        _previewHint.Name = "previewHint";

        _videoPanel.Controls.AddRange(new Control[] { _videoTitle, _videoPathLabel, _videoInfoLabel, _previewButton, _previewHint });
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
        _cropKeepAspect.Checked = s.CropKeepAspect;
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
        // Start from what is on disk: autostart and the section that is not being
        // edited right now stay untouched; "Сохранить" writes both sections + type.
        var settings = Settings.Load();

        if (CurrentSourceType == SourceType.Video)
        {
            if (string.IsNullOrEmpty(_videoPath))
            {
                MessageBox.Show(this, "Сначала выберите видеоролик.", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Information);
                return;
            }
            if (!File.Exists(_videoPath))
            {
                MessageBox.Show(this, "Файл ролика не найден — выберите его заново.", Text,
                    MessageBoxButtons.OK, MessageBoxIcon.Warning);
                return;
            }

            settings.VideoPath = _videoPath;
            settings.SourceType = SourceType.Video;
            settings.Quality = CurrentQuality;
            TrySave(settings,
                $"Сохранено: {Settings.FilePath} — хост подхватит source.type/video.path (~1 с).");
            return;
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
                TrySave(settings,
                    $"Сохранено: {Settings.FilePath} — камера НЕ выбрана, хост будет показывать NO SIGNAL, пока вы не выберете устройство.",
                    warn: true);
            }
            else
            {
                TrySave(settings,
                    $"Сохранено: {Settings.FilePath} — хост подхватит source.type/camera (~1 с): {cam.Name}.");
            }
            return;
        }

        if (string.IsNullOrEmpty(_sourcePath))
        {
            MessageBox.Show(this, "Сначала выберите картинку.", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
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

        TrySave(settings,
            $"Сохранено: {Settings.FilePath} — хост подхватит source.type/static (~1 с).");
    }

    private void TrySave(Settings settings, string okText, bool warn = false)
    {
        try
        {
            settings.Save();
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
        _sourceImage?.Dispose();
        var img = _preview.Image;
        _preview.Image = null;
        img?.Dispose();
        base.OnFormClosed(e);
    }
}
