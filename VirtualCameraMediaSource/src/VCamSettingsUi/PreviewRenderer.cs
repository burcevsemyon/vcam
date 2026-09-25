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
}
