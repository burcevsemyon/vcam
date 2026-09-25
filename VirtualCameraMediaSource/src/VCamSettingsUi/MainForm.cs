namespace VCamSettingsUi;

public sealed class MainForm : Form
{
    private readonly PictureBox _preview = new();
    private readonly ComboBox _mode = new();
    private readonly Button _openButton = new();
    private readonly Button _fullSizeButton = new();
    private readonly Button _saveButton = new();
    private readonly Label _pathLabel = new();
    private readonly Label _hintLabel = new();

    private Image? _sourceImage;
    private string _sourcePath = "";

    public MainForm()
    {
        Text = "VCam — настройки трансляции";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(880, 620);
        Font = new Font("Segoe UI", 9f);

        _preview.Location = new Point(12, 12);
        _preview.Size = new Size(856, 481); // 16:9 for 1280x720
        _preview.SizeMode = PictureBoxSizeMode.Zoom;
        _preview.BackColor = Color.Black;
        _preview.BorderStyle = BorderStyle.FixedSingle;

        _pathLabel.Location = new Point(12, 500);
        _pathLabel.Size = new Size(856, 20);
        _pathLabel.Text = "(файл не выбран)";
        _pathLabel.AutoEllipsis = true;

        _mode.Location = new Point(12, 526);
        _mode.Size = new Size(160, 28);
        _mode.DropDownStyle = ComboBoxStyle.DropDownList;
        _mode.Items.AddRange(new object[] { "fit — вписать с полосами", "cover — заполнить (обрезка)" });
        _mode.SelectedIndex = 0;

        _openButton.Location = new Point(180, 524);
        _openButton.Size = new Size(160, 30);
        _openButton.Text = "Открыть картинку…";
        _openButton.Click += OnOpenClicked;

        _fullSizeButton.Location = new Point(348, 524);
        _fullSizeButton.Size = new Size(160, 30);
        _fullSizeButton.Text = "Просмотр полный";
        _fullSizeButton.Enabled = false;
        _fullSizeButton.Click += OnFullSizeClicked;

        _saveButton.Location = new Point(680, 524);
        _saveButton.Size = new Size(188, 30);
        _saveButton.Text = "Сохранить настройки";
        _saveButton.Click += OnSaveClicked;

        _hintLabel.Location = new Point(12, 564);
        _hintLabel.Size = new Size(856, 40);
        _hintLabel.ForeColor = Color.DimGray;
        _hintLabel.Text = $"Настройки: {Settings.FilePath} — StaticProducer подхватит их автоматически (~0.7 с).";

        Controls.AddRange(new Control[] { _preview, _pathLabel, _mode, _openButton, _fullSizeButton, _saveButton, _hintLabel });

        _mode.SelectedIndexChanged += (_, _) => UpdatePreview();

        LoadCurrentSettings();
    }

    private void LoadCurrentSettings()
    {
        var s = Settings.Load();
        _mode.SelectedIndex = s.ScaleMode == ScaleMode.Cover ? 1 : 0;
        if (!string.IsNullOrEmpty(s.ImagePath) && File.Exists(s.ImagePath))
        {
            SetSource(s.ImagePath);
        }
    }

    private void OnOpenClicked(object? sender, EventArgs e)
    {
        using var dlg = new OpenFileDialog
        {
            Title = "Выберите изображение",
            Filter = "Изображения|*.png;*.jpg;*.jpeg;*.jfif;*.bmp;*.gif;*.tif;*.tiff;*.webp|Все файлы|*.*",
        };
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        SetSource(dlg.FileName);
    }

    private void SetSource(string path)
    {
        try
        {
            // Load via stream so the file stays unlocked for other processes.
            using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var img = Image.FromStream(fs);
            _sourceImage?.Dispose();
            _sourceImage = new Bitmap(img); // detach from the stream
            img.Dispose();
            _sourcePath = path;
            _pathLabel.Text = path;
            _fullSizeButton.Enabled = true;
            UpdatePreview();
        }
        catch (Exception ex)
        {
            MessageBox.Show(this, $"Не удалось открыть изображение:\n{ex.Message}", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Error);
        }
    }

    private ScaleMode CurrentMode => _mode.SelectedIndex == 1 ? ScaleMode.Cover : ScaleMode.Fit;

    private void UpdatePreview()
    {
        if (_sourceImage is null) return;
        var old = _preview.Image;
        _preview.Image = PreviewRenderer.Render(_sourceImage, CurrentMode);
        old?.Dispose();
    }

    private void OnFullSizeClicked(object? sender, EventArgs e)
    {
        if (_sourceImage is null) return;
        new FullSizeForm(_sourceImage, _sourcePath).Show(this);
    }

    private void OnSaveClicked(object? sender, EventArgs e)
    {
        if (string.IsNullOrEmpty(_sourcePath))
        {
            MessageBox.Show(this, "Сначала выберите картинку.", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        var settings = new Settings { ImagePath = _sourcePath, ScaleMode = CurrentMode };
        try
        {
            settings.Save();
            _hintLabel.ForeColor = Color.ForestGreen;
            _hintLabel.Text = $"Сохранено: {Settings.FilePath} — StaticProducer применит автоматически (~0.7 с).";
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
