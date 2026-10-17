using Xunit;

namespace VCamUiTests;

// Регрессия: переключатель fit/cover/crop на форме (баг 05.10.2026 —
// переключение ничего не меняло в превью). fit/cover обязаны давать разную
// картинку, crop — подменять превью на редактор области.
[Collection("UiApp")]
public sealed class ModeSwitchTests(UiAppFixture fx)
{
    private static bool Hidden(FlaUI.Core.AutomationElements.AutomationElement? el) =>
        el is null || el.IsOffscreen;

    private static void WaitWhile(Func<bool> stillBad, TimeSpan timeout, string what)
    {
        var sw = System.Diagnostics.Stopwatch.StartNew();
        while (sw.Elapsed < timeout)
        {
            if (!stillBad()) return;
            Thread.Sleep(200);
        }
        throw new TimeoutException($"Не дождались: {what}");
    }

    [Fact]
    public void FitVsCover_preview_differs()
    {
        fx.SelectMode(0); // fit
        WaitWhile(() => Hidden(fx.FindById("preview")), TimeSpan.FromSeconds(5), "preview(fit)");
        using var fit = Screenshots.Capture(fx.FindById("preview")!.BoundingRectangle);
        string fitHash = Screenshots.Hash(fit);
        Assert.False(Screenshots.IsSolidBlack(fit), "fit-превью чёрное: картинка не загрузилась.");

        fx.SelectMode(1); // cover
        WaitWhile(() => Hidden(fx.FindById("preview")), TimeSpan.FromSeconds(5), "preview(cover)");
        using var cover = Screenshots.Capture(fx.FindById("preview")!.BoundingRectangle);
        string coverHash = Screenshots.Hash(cover);
        Assert.False(Screenshots.IsSolidBlack(cover), "cover-превью чёрное.");

        Assert.NotEqual(fitHash, coverHash); // 800x600 в 16:9: полосы vs обрезка
    }

    [Fact]
    public void Crop_shows_editor_and_hides_preview()
    {
        fx.SelectMode(2); // crop
        WaitWhile(() => !Hidden(fx.FindById("preview"))
            || Hidden(fx.FindByName("Сохранять пропорции")),
            TimeSpan.FromSeconds(5), "crop-редактор");

        Assert.True(Hidden(fx.FindById("preview")), "preview не скрылось в crop-режиме.");
        var keepAspect = fx.FindByName("Сохранять пропорции");
        Assert.True(keepAspect is not null && !keepAspect.IsOffscreen,
            "Поля crop-области не показались.");
    }

    [Fact]
    public void BackToFit_restores_preview()
    {
        fx.SelectMode(2); // crop
        fx.SelectMode(0); // fit
        WaitWhile(() => Hidden(fx.FindById("preview")), TimeSpan.FromSeconds(5), "preview(fit)");
        using var shot = Screenshots.Capture(fx.FindById("preview")!.BoundingRectangle);
        Assert.False(Screenshots.IsSolidBlack(shot), "Возврат в fit не вернул картинку.");
    }
}
