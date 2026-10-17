using System.ComponentModel;

namespace VCamSettingsUi;

// Interactive crop selection over the source image (shown fit-to-control with letterboxing).
// Mouse: drag handles = resize, drag inside = move, drag outside = new selection.
// Selection is kept in SOURCE image pixels; must be read/written as settings cropX/Y/W/H.
public sealed class CropPreviewControl : Control
{
    private const int HandleSize = 8;
    private const int HitSlop = 6;
    private const int MinSize = 16;

    private Image? _image;
    private RectangleF _displayRect;
    private Rectangle _selection; // source px

    private enum Drag { None, Move, NW, N, NE, E, SE, S, SW, W, New }
    private Drag _drag = Drag.None;
    private Point _dragAnchorSrc;   // source px where current drag started
    private Rectangle _dragStartSel; // selection at drag start

    public event Action? SelectionChanged;

    public CropPreviewControl()
    {
        SetStyle(ControlStyles.AllPaintingInWmPaint | ControlStyles.OptimizedDoubleBuffer |
                  ControlStyles.UserPaint | ControlStyles.ResizeRedraw, true);
        BackColor = Color.Black;
        TabStop = true;
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    [Browsable(false)]
    public Image? Image
    {
        get => _image;
        set
        {
            _image = value;
            RecalcDisplay();
            Selection = value is null ? Rectangle.Empty : PreviewRenderer.DefaultCrop(value);
            Invalidate();
        }
    }

    [DesignerSerializationVisibility(DesignerSerializationVisibility.Hidden)]
    [Browsable(false)]
    public Rectangle Selection
    {
        get => _selection;
        set
        {
            _selection = value;
            Invalidate();
            SelectionChanged?.Invoke();
        }
    }

    private void RecalcDisplay()
    {
        if (_image is null || ClientSize.Width <= 0 || ClientSize.Height <= 0)
        {
            _displayRect = RectangleF.Empty;
            return;
        }
        float scale = Math.Min(ClientSize.Width / (float)_image.Width, ClientSize.Height / (float)_image.Height);
        float w = _image.Width * scale;
        float h = _image.Height * scale;
        _displayRect = new RectangleF((ClientSize.Width - w) / 2f, (ClientSize.Height - h) / 2f, w, h);
    }

    protected override void OnResize(EventArgs e)
    {
        base.OnResize(e);
        RecalcDisplay();
    }

    // --- coordinate mapping -------------------------------------------------

    private Point SrcToCtrl(Point src)
    {
        if (_image is null) return Point.Empty;
        return new Point(
            (int)Math.Round(_displayRect.X + src.X * _displayRect.Width / _image.Width),
            (int)Math.Round(_displayRect.Y + src.Y * _displayRect.Height / _image.Height));
    }

    private Point CtrlToSrc(Point p)
    {
        if (_image is null) return Point.Empty;
        float sx = (p.X - _displayRect.X) * _image.Width / _displayRect.Width;
        float sy = (p.Y - _displayRect.Y) * _image.Height / _displayRect.Height;
        return new Point(
            Math.Clamp((int)Math.Round(sx), 0, _image.Width),
            Math.Clamp((int)Math.Round(sy), 0, _image.Height));
    }

    private Rectangle SelInCtrl()
    {
        var tl = SrcToCtrl(new Point(_selection.X, _selection.Y));
        var br = SrcToCtrl(new Point(_selection.Right, _selection.Bottom));
        return Rectangle.FromLTRB(tl.X, tl.Y, br.X, br.Y);
    }

    private IEnumerable<(Drag handle, Rectangle rect)> HandleRects()
    {
        var r = SelInCtrl();
        int hw = r.Width / 2;
        int hh = r.Height / 2;
        yield return (Drag.NW, HandleRect(r.X, r.Y));
        yield return (Drag.N, HandleRect(r.X + hw - HandleSize / 2, r.Y));
        yield return (Drag.NE, HandleRect(r.Right - HandleSize, r.Y));
        yield return (Drag.E, HandleRect(r.Right - HandleSize, r.Y + hh - HandleSize / 2));
        yield return (Drag.SE, HandleRect(r.Right - HandleSize, r.Bottom - HandleSize));
        yield return (Drag.S, HandleRect(r.X + hw - HandleSize / 2, r.Bottom - HandleSize));
        yield return (Drag.SW, HandleRect(r.X, r.Bottom - HandleSize));
        yield return (Drag.W, HandleRect(r.X, r.Y + hh - HandleSize / 2));
    }

    private static Rectangle HandleRect(int x, int y) => new Rectangle(x - HitSlop, y - HitSlop, HandleSize + 2 * HitSlop, HandleSize + 2 * HitSlop);

    private Drag HitTest(Point ctrlP)
    {
        foreach (var (handle, rect) in HandleRects())
            if (rect.Contains(ctrlP)) return handle;
        if (SelInCtrl().Contains(ctrlP)) return Drag.Move;
        return _displayRect.Contains(ctrlP) ? Drag.New : Drag.None;
    }

    // --- mouse --------------------------------------------------------------

    protected override void OnMouseDown(MouseEventArgs e)
    {
        base.OnMouseDown(e);
        if (_image is null) return;
        Focus();
        _drag = HitTest(e.Location);
        _dragAnchorSrc = CtrlToSrc(e.Location);
        _dragStartSel = _selection;
        if (_drag == Drag.New)
        {
            _selection = new Rectangle(_dragAnchorSrc, Size.Empty);
            SelectionChanged?.Invoke();
        }
    }

    protected override void OnMouseMove(MouseEventArgs e)
    {
        base.OnMouseMove(e);
        if (_image is null) return;

        if (_drag == Drag.None)
        {
            Cursor = HitTest(e.Location) switch
            {
                Drag.Move => Cursors.SizeAll,
                Drag.N or Drag.S => Cursors.SizeNS,
                Drag.E or Drag.W => Cursors.SizeWE,
                Drag.NW or Drag.SE => Cursors.SizeNWSE,
                Drag.NE or Drag.SW => Cursors.SizeNESW,
                Drag.New => Cursors.Cross,
                _ => Cursors.Default,
            };
            return;
        }

        var cur = CtrlToSrc(e.Location);
        var sel = _drag == Drag.New ? new Rectangle(_dragAnchorSrc, Size.Empty) : _dragStartSel;

        switch (_drag)
        {
            case Drag.Move:
            {
                int dx = cur.X - _dragAnchorSrc.X;
                int dy = cur.Y - _dragAnchorSrc.Y;
                int x = Math.Clamp(_dragStartSel.X + dx, 0, _image.Width - _dragStartSel.Width);
                int y = Math.Clamp(_dragStartSel.Y + dy, 0, _image.Height - _dragStartSel.Height);
                sel = new Rectangle(x, y, _dragStartSel.Width, _dragStartSel.Height);
                break;
            }
            case Drag.NE: sel = new Rectangle(sel.X, cur.Y, Math.Max(MinSize, cur.X - sel.X), Math.Max(MinSize, sel.Bottom - cur.Y)); break;
            case Drag.N: sel = new Rectangle(sel.X, cur.Y, sel.Width, Math.Max(MinSize, sel.Bottom - cur.Y)); break;
            case Drag.NW: sel = new Rectangle(cur.X, cur.Y, Math.Max(MinSize, sel.Right - cur.X), Math.Max(MinSize, sel.Bottom - cur.Y)); break;
            case Drag.W: sel = new Rectangle(cur.X, sel.Y, Math.Max(MinSize, sel.Right - cur.X), sel.Height); break;
            case Drag.SW: sel = new Rectangle(cur.X, sel.Y, Math.Max(MinSize, sel.Right - cur.X), Math.Max(MinSize, cur.Y - sel.Y)); break;
            case Drag.S: sel = new Rectangle(sel.X, sel.Y, sel.Width, Math.Max(MinSize, cur.Y - sel.Y)); break;
            case Drag.SE: sel = new Rectangle(sel.X, sel.Y, Math.Max(MinSize, cur.X - sel.X), Math.Max(MinSize, cur.Y - sel.Y)); break;
            case Drag.E: sel = new Rectangle(sel.X, sel.Y, Math.Max(MinSize, cur.X - sel.X), sel.Height); break;
            case Drag.New: sel = Rectangle.FromLTRB(_dragAnchorSrc.X, _dragAnchorSrc.Y, cur.X, cur.Y); break;
        }

        // Clamp to source bounds.
        sel.X = Math.Max(0, Math.Min(sel.X, _image.Width - 1));
        sel.Y = Math.Max(0, Math.Min(sel.Y, _image.Height - 1));
        sel.Width = Math.Max(MinSize, Math.Min(sel.Width, _image.Width - sel.X));
        sel.Height = Math.Max(MinSize, Math.Min(sel.Height, _image.Height - sel.Y));

        _selection = sel;
        SelectionChanged?.Invoke();
        Invalidate();
    }

    protected override void OnMouseUp(MouseEventArgs e)
    {
        base.OnMouseUp(e);
        if (_image is null) return;
        if (_drag == Drag.New && (_selection.Width < MinSize || _selection.Height < MinSize))
        {
            _selection = _dragStartSel; // treat tiny drag as click: keep old selection
        }
        _drag = Drag.None;
        Invalidate();
        SelectionChanged?.Invoke();
    }

    // --- paint --------------------------------------------------------------

    protected override void OnPaint(PaintEventArgs e)
    {
        base.OnPaint(e);
        var g = e.Graphics;
        g.Clear(BackColor);
        if (_image is null) return;

        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
        g.DrawImage(_image, _displayRect);

        var sel = SelInCtrl();

        // Dim everything outside the selection.
        using (var dim = new SolidBrush(Color.FromArgb(150, Color.Black)))
        {
            var top = new RectangleF(0, 0, ClientSize.Width, Math.Max(0, sel.Top));
            var bottom = new RectangleF(0, sel.Bottom, ClientSize.Width, Math.Max(0, ClientSize.Height - sel.Bottom));
            var left = new RectangleF(0, sel.Top, Math.Max(0, sel.Left), sel.Height);
            var right = new RectangleF(sel.Right, sel.Top, Math.Max(0, ClientSize.Width - sel.Right), sel.Height);
            g.FillRectangle(dim, top);
            g.FillRectangle(dim, bottom);
            g.FillRectangle(dim, left);
            g.FillRectangle(dim, right);
        }

        using var pen = new Pen(Color.FromArgb(0, 255, 128), 2f);
        g.DrawRectangle(pen, sel);

        using var fill = new SolidBrush(Color.FromArgb(0, 255, 128));
        foreach (var (_, rect) in HandleRects())
        {
            var visible = new Rectangle(rect.X + HitSlop, rect.Y + HitSlop, HandleSize, HandleSize);
            g.FillRectangle(fill, visible);
        }

        // Size label near the top-left corner of the selection.
        string label = $"{_selection.Width}×{_selection.Height}";
        using var font = new Font("Segoe UI", 9f, FontStyle.Bold);
        var sz = g.MeasureString(label, font);
        var lp = new PointF(sel.X + 4, Math.Max(2, sel.Y - sz.Height - 4));
        if (lp.Y + sz.Height > sel.Y) lp.Y = sel.Y + 4;
        using var bg = new SolidBrush(Color.FromArgb(180, Color.Black));
        g.FillRectangle(bg, lp.X - 3, lp.Y - 1, sz.Width + 6, sz.Height + 2);
        g.DrawString(label, font, Brushes.White, lp);
    }

    protected override void Dispose(bool disposing)
    {
        if (disposing) _image = null;
        base.Dispose(disposing);
    }
}
