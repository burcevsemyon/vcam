namespace VCamSettingsUi;

// Preview rendering must stay in sync with LoadAndScaleImage in
// src/StaticProducer/StaticProducer.cpp (Fit = letterbox on black, Cover = center-crop fill).
public static class PreviewRenderer
{
    public const int TargetWidth = 1280;
    public const int TargetHeight = 720;

    public static Image Render(Image source, ScaleMode mode, int targetW = TargetWidth, int targetH = TargetHeight)
    {
        var bmp = new Bitmap(targetW, targetH);
        using var g = Graphics.FromImage(bmp);
        g.Clear(Color.Black);
        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic; // GDI+ equivalent of WIC HighQualityCubic
        g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;

        double scale = mode == ScaleMode.Fit
            ? Math.Min((double)targetW / source.Width, (double)targetH / source.Height)
            : Math.Max((double)targetW / source.Width, (double)targetH / source.Height);

        int nw = Math.Max(1, (int)Math.Round(source.Width * scale));
        int nh = Math.Max(1, (int)Math.Round(source.Height * scale));
        if (mode == ScaleMode.Fit) { nw = Math.Min(nw, targetW); nh = Math.Min(nh, targetH); }
        else { nw = Math.Max(nw, targetW); nh = Math.Max(nh, targetH); }

        // Single draw call covers both modes: the Graphics surface (targetW x targetH)
        // clips overflow automatically - letterbox padding stays black (Fit) and the
        // scaled-up image is center-cropped at the edges (Cover).
        int x = (targetW - nw) / 2;
        int y = (targetH - nh) / 2;
        g.DrawImage(source, x, y, nw, nh);

        return bmp;
    }

    // Crop: clip source rect (source pixels) and stretch it to the full target
    // (aspect NOT preserved) - must stay in sync with the Crop branch in
    // src/StaticProducer/StaticProducer.cpp LoadAndScaleImage.
    // keepAspect = true: letterbox the region instead (also mirrored there).
    public static Image RenderCrop(Image source, Rectangle crop, bool keepAspect,
                                   int targetW = TargetWidth, int targetH = TargetHeight)
    {
        var clamped = ClampCrop(source, crop);
        var bmp = new Bitmap(targetW, targetH);
        using var g = Graphics.FromImage(bmp);
        g.Clear(Color.Black);
        g.InterpolationMode = System.Drawing.Drawing2D.InterpolationMode.HighQualityBicubic;
        g.PixelOffsetMode = System.Drawing.Drawing2D.PixelOffsetMode.HighQuality;

        if (keepAspect)
        {
            double scale = Math.Min((double)targetW / clamped.Width, (double)targetH / clamped.Height);
            int nw = Math.Max(1, Math.Min(targetW, (int)Math.Round(clamped.Width * scale)));
            int nh = Math.Max(1, Math.Min(targetH, (int)Math.Round(clamped.Height * scale)));
            g.DrawImage(source, new Rectangle((targetW - nw) / 2, (targetH - nh) / 2, nw, nh), clamped, GraphicsUnit.Pixel);
        }
        else
        {
            g.DrawImage(source, new Rectangle(0, 0, targetW, targetH), clamped, GraphicsUnit.Pixel);
        }
        return bmp;
    }

    public static Rectangle ClampCrop(Image source, Rectangle crop)
    {
        int x = Math.Max(0, Math.Min(crop.X, source.Width - 1));
        int y = Math.Max(0, Math.Min(crop.Y, source.Height - 1));
        int w = crop.Width <= 0 ? source.Width : Math.Min(crop.Width, source.Width - x);
        int h = crop.Height <= 0 ? source.Height : Math.Min(crop.Height, source.Height - y);
        if (w <= 0) w = source.Width;
        if (h <= 0) h = source.Height;
        return new Rectangle(x, y, w, h);
    }

    // Default selection: centered rect with the target 16:9 aspect (what Cover would keep).
    public static Rectangle DefaultCrop(Image source)
    {
        const double aspect = (double)TargetWidth / TargetHeight;
        int w = source.Width;
        int h = (int)Math.Round(w / aspect);
        if (h > source.Height)
        {
            h = source.Height;
            w = (int)Math.Round(h * aspect);
        }
        return new Rectangle((source.Width - w) / 2, (source.Height - h) / 2, w, h);
    }
}
