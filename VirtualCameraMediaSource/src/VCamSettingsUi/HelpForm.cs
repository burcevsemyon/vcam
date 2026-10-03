// Модальное окно справки (тексты — HelpTexts). Тот же визуальный стиль,
// что и остальные панели: белый фон, Segoe UI 9.

namespace VCamSettingsUi;

public sealed class HelpForm : Form
{
    public HelpForm()
    {
        Text = HelpTexts.Title;
        FormBorderStyle = FormBorderStyle.FixedDialog;
        MaximizeBox = false;
        MinimizeBox = false;
        StartPosition = FormStartPosition.CenterParent;
        ClientSize = new Size(640, 520);
        Font = new Font("Segoe UI", 9f);
        Name = "helpForm";

        var title = new Label
        {
            Location = new Point(16, 12),
            Size = new Size(608, 26),
            Font = new Font(Font.FontFamily, 11f, FontStyle.Bold),
            Text = HelpTexts.Title,
            Name = "helpTitle",
        };

        var body = new TextBox
        {
            Location = new Point(16, 44),
            Size = new Size(608, 412),
            Multiline = true,
            ReadOnly = true,
            ScrollBars = ScrollBars.Vertical,
            BorderStyle = BorderStyle.FixedSingle,
            BackColor = Color.White,
            Name = "helpBody",
            Text = BuildBody(),
        };

        var ok = new Button
        {
            Location = new Point(484, 468),
            Size = new Size(140, 36),
            Text = "Понятно",
            DialogResult = DialogResult.OK,
            Name = "helpOk",
        };
        AcceptButton = ok;

        Controls.AddRange(new Control[] { title, body, ok });
    }

    private static string BuildBody()
    {
        var sb = new System.Text.StringBuilder();
        foreach (var (heading, body) in HelpTexts.Sections)
            sb.AppendLine(heading).AppendLine(body).AppendLine();
        return sb.ToString();
    }
}
