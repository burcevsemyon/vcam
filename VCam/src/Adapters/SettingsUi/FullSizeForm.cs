namespace VCamSettingsUi;

// Full-resolution viewer: scrollable 1:1 image, mouse wheel zoom.
public sealed class FullSizeForm : Form
{
    private readonly PictureBox _picture = new();
    private readonly Panel _panel = new();
    private readonly Label _status = new();
    private readonly Image _image;
    private readonly string _path;
    private int _zoom = 100;

    public FullSizeForm(Image image, string path)
    {
        _image = image;
        _path = path;

        Text = string.IsNullOrEmpty(path) ? "Просмотр" : $"Просмотр — {Path.GetFileName(path)}";
        StartPosition = FormStartPosition.CenterScreen;
        ClientSize = new Size(1000, 700);
        Font = new Font("Segoe UI", 9f);

        _panel.Dock = DockStyle.Fill;
        _panel.AutoScroll = true;
        _panel.BackColor = Color.FromArgb(32, 32, 32);

        _picture.SizeMode = PictureBoxSizeMode.AutoSize;
        _picture.Location = Point.Empty;
        _picture.Image = image;

        _status.Dock = DockStyle.Bottom;
        _status.Height = 24;
        _status.BackColor = Color.FromArgb(48, 48, 48);
        _status.ForeColor = Color.Gainsboro;
        _status.TextAlign = ContentAlignment.MiddleLeft;
        _status.Padding = new Padding(6, 0, 0, 0);

        _panel.Controls.Add(_picture);
        Controls.Add(_panel);
        Controls.Add(_status);

        UpdateStatus();
        MouseWheel += OnMouseWheel;
        _picture.MouseWheel += OnMouseWheel;
    }

    private void UpdateStatus()
    {
        _status.Text = $"{_image.Width}×{_image.Height}   зум {_zoom}%   (колесо — масштаб, 100% = оригинал)";
    }

    private void OnMouseWheel(object? sender, MouseEventArgs e)
    {
        int step = e.Delta > 0 ? 10 : -10;
        int next = Math.Clamp(_zoom + step, 25, 400);
        if (next == _zoom) return;
        _zoom = next;
        ApplyZoom();
    }

    private void ApplyZoom()
    {
        int w = Math.Max(1, _image.Width * _zoom / 100);
        int h = Math.Max(1, _image.Height * _zoom / 100);
        _picture.SizeMode = PictureBoxSizeMode.StretchImage;
        _picture.Size = new Size(w, h);
        UpdateStatus();
    }
}
