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
    private bool _updatingCropFields;

    private static readonly string[] ModeNames = { "fit — вписать с пололосами", "cover — заполнить (обрезка)", "crop — обрезка выбранной области" };

    public MainForm()
    {
        Text = "VCam — настройки трансляции";
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(880, 656);
        Font = new Font("Segoe UI", 9f);

        _preview.Location = new Point(12, 12);
        _preview.Size = new Size(856, 481); // 16:9 for 1280x720
        _preview.SizeMode = PictureBoxSizeMode.Zoom;
        _preview.BackColor = Color.Black;
        _preview.BorderStyle = BorderStyle.FixedSingle;

        _cropView.Location = _preview.Location;
        _cropView.Size = _preview.Size;
        _cropView.Visible = false;
        _cropView.SelectionChanged += OnCropSelectionChanged;

        _pathLabel.Location = new Point(12, 500);
        _pathLabel.Size = new Size(856, 20);
        _pathLabel.Text = "(файл не выбран)";
        _pathLabel.AutoEllipsis = true;

        _mode.Location = new Point(12, 526);
        _mode.Size = new Size(250, 28);
        _mode.DropDownStyle = ComboBoxStyle.DropDownList;
        _mode.Items.AddRange(ModeNames);
        _mode.SelectedIndex = 0;

        _openButton.Location = new Point(268, 524);
        _openButton.Size = new Size(160, 30);
        _openButton.Text = "Открыть картинку…";
        _openButton.Click += OnOpenClicked;

        _fullSizeButton.Location = new Point(436, 524);
        _fullSizeButton.Size = new Size(150, 30);
        _fullSizeButton.Text = "Просмотр полный";
        _fullSizeButton.Enabled = false;
        _fullSizeButton.Click += OnFullSizeClicked;

        _saveButton.Location = new Point(680, 524);
        _saveButton.Size = new Size(188, 30);
        _saveButton.Text = "Сохранить настройки";
        _saveButton.Click += OnSaveClicked;

        int fieldY = 562;
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

        _hintLabel.Location = new Point(12, 596);
        _hintLabel.Size = new Size(856, 52);
        _hintLabel.ForeColor = Color.DimGray;
        _hintLabel.Text = $"Настройки: {Settings.FilePath} — StaticProducer подхватит их автоматически (~0.7 с).";

        Controls.AddRange(new Control[] { _preview, _cropView, _pathLabel, _mode, _openButton, _fullSizeButton, _saveButton,
            _cropXLabel, _cropX, _cropYLabel, _cropY, _cropWLabel, _cropW, _cropHLabel, _cropH, _cropKeepAspect, _hintLabel });

        _mode.SelectedIndexChanged += (_, _) => OnModeChanged();

        LoadCurrentSettings();
        OnModeChanged();
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
        if (!string.IsNullOrEmpty(s.ImagePath) && File.Exists(s.ImagePath))
        {
            SetSource(s.ImagePath, new Rectangle(s.CropX, s.CropY, s.CropW, s.CropH));
        }
    }

    private ScaleMode CurrentMode => _mode.SelectedIndex switch
    {
        1 => ScaleMode.Cover,
        2 => ScaleMode.Crop,
        _ => ScaleMode.Fit,
    };

    private void OnModeChanged()
    {
        bool crop = CurrentMode == ScaleMode.Crop;
        _cropView.Visible = crop;
        _preview.Visible = !crop;
        foreach (var c in new Control[] { _cropXLabel, _cropX, _cropYLabel, _cropY, _cropWLabel, _cropW, _cropHLabel, _cropH, _cropKeepAspect })
            c.Visible = crop;
        if (crop && _sourceImage != null && _cropView.Image == null)
        {
            _cropView.Image = _sourceImage; // sets default selection + fires SelectionChanged
        }
        if (!crop) UpdatePreview();
        else UpdateCropFieldsFromSelection();
    }

    private void OnOpenClicked(object? sender, EventArgs e)
    {
        using var dlg = new OpenFileDialog
        {
            Title = "Выберите изображение",
            Filter = "Изображения|*.png;*.jpg;*.jpeg;*.jfif;*.bmp;*.gif;*.tif;*.tiff;*.webp|Все файлы|*.*",
        };
        if (dlg.ShowDialog(this) != DialogResult.OK) return;
        SetSource(dlg.FileName, null);
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

    private void OnSaveClicked(object? sender, EventArgs e)
    {
        if (string.IsNullOrEmpty(_sourcePath))
        {
            MessageBox.Show(this, "Сначала выберите картинку.", Text,
                MessageBoxButtons.OK, MessageBoxIcon.Information);
            return;
        }

        var settings = new Settings
        {
            ImagePath = _sourcePath,
            ScaleMode = CurrentMode,
        };
        if (CurrentMode == ScaleMode.Crop && _sourceImage != null)
        {
            var sel = PreviewRenderer.ClampCrop(_sourceImage, _cropView.Selection);
            settings.CropX = sel.X;
            settings.CropY = sel.Y;
            settings.CropW = sel.Width;
            settings.CropH = sel.Height;
            settings.CropKeepAspect = _cropKeepAspect.Checked;
        }

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
