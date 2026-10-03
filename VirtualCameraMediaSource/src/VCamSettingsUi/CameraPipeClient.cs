// Управляющий канал камеры: C# клиент pipe `\\.\pipe\VCamControl.v1`.
// Протокол — src/ProducerCore/ControlServer.h (JSON UTF-8, один запрос —
// одна строка `\n`, один ответ — одна строка). Опсы list/get/set.
// Сервер живёт ТОЛЬКО при открытом source=camera; отсутствие сервера —
// штатная ситуация (методы возвращают false + текст, исключений наружу нет).
// Ручки — живые контролы, в settings.json НЕ сохраняются.

using System.IO.Pipes;
using System.Text;
using System.Text.Json;

namespace VCamSettingsUi;

// Одна ручка физической камеры (элемент ответа list).
public sealed class CameraControlInfo
{
    public string Domain { get; set; } = ""; // "procamp" | "camera"
    public long Id { get; set; }
    public string Name { get; set; } = "";  // "brightness", "exposure", ...
    public long Min { get; set; }
    public long Max { get; set; }
    public long Step { get; set; } = 1;
    public long Def { get; set; }
    public long Caps { get; set; }
    public long Cur { get; set; }
    public long Flags { get; set; } // 0x1 = авто, 0x2 = вручную
    public bool Supported { get; set; }

    public string Key => $"{Domain}:{Name}".ToLowerInvariant();
    public bool IsAuto => (Flags & 1) != 0;

    // Обновление из свежего снимка (опрос): true = значение/режим изменились.
    public bool ApplySnapshot(long cur, long flags)
    {
        if (cur == Cur && flags == Flags) return false;
        Cur = cur;
        Flags = flags;
        return true;
    }
}

// Русские названия и простые пояснения ручек (без жаргона).
public static class CameraControlTexts
{
    private static readonly Dictionary<string, (string Title, string About)> Map = new()
    {
        ["procamp:brightness"] = ("Яркость", "Общая светлота картинки."),
        ["procamp:contrast"] = ("Контрастность", "Разница между тёмным и светлым."),
        ["procamp:hue"] = ("Оттенок", "Сдвиг цветов."),
        ["procamp:saturation"] = ("Насыщенность", "Сочность цвета."),
        ["procamp:sharpness"] = ("Чёткость", "Резкость контуров."),
        ["procamp:gamma"] = ("Гамма", "Полутона, детали в тенях."),
        ["procamp:whitebalance"] = ("Баланс белого", "Теплота света: чтобы белое выглядело белым."),
        ["procamp:backlightcompensation"] = ("Свет сзади", "Компенсация яркого света позади вас (вкл/выкл)."),
        ["procamp:gain"] = ("Усиление", "Электронное усиление: ярче, но больше шума."),
        ["camera:pan"] = ("Поворот", "Поворот камеры влево-вправо."),
        ["camera:tilt"] = ("Наклон", "Наклон камеры вверх-вниз."),
        ["camera:roll"] = ("Крен", "Поворот кадра."),
        ["camera:zoom"] = ("Приближение", "Приближение картинки."),
        ["camera:exposure"] = ("Выдержка", "Яркость при слабом свете."),
        ["camera:iris"] = ("Диафрагма", "Количество света через объектив."),
        ["camera:focus"] = ("Фокус", "Резкость на вашем расстоянии."),
    };

    public static string TitleOf(CameraControlInfo c) =>
        Map.TryGetValue(c.Key, out var t) ? t.Title : c.Name;

    public static string AboutOf(CameraControlInfo c) =>
        Map.TryGetValue(c.Key, out var t) ? t.About : "";

    public static string ModeMark(CameraControlInfo c)
    {
        if (!c.Supported) return "—";
        return c.IsAuto ? "авто" : "вручную";
    }

    public static string ToolTipOf(CameraControlInfo c)
    {
        var sb = new StringBuilder();
        if (Map.TryGetValue(c.Key, out var t) && t.About.Length > 0)
            sb.Append(t.About).Append(' ');
        sb.Append($"Диапазон {c.Min}…{c.Max}, шаг {c.Step}, по умолчанию {c.Def}.");
        if (!c.Supported)
            sb.Append(" Камера это не умеет — строка серая, это нормально.");
        else
            sb.Append(" Двойной щелчок по ползунку возвращает значение по умолчанию.");
        return sb.ToString();
    }
}

// Построение запросов и разбор ответов (чистые функции — покрыты тестами).
public static class CameraControlProtocol
{
    public const string PipeName = "VCamControl.v1";

    public static string BuildListRequest() => """{"op":"list"}""";

    public static string BuildGetRequest(string domain, string name) =>
        $$"""{"op":"get","domain":"{{domain}}","id":"{{name}}"}""";

    public static string BuildSetRequest(string domain, string name, long value, long flags = 2) =>
        $$"""{"op":"set","domain":"{{domain}}","id":"{{name}}","value":{{value}},"flags":{{flags}}}""";

    // Разбор ответа list. ok=false или мусор → false + текст ошибки.
    public static bool TryParseList(string response, out List<CameraControlInfo> controls, out string error)
    {
        controls = new List<CameraControlInfo>();
        error = "";
        JsonDocument doc;
        try { doc = JsonDocument.Parse(response); }
        catch (Exception) { error = "ответ не похож на JSON"; return false; }
        using (doc)
        {
            var root = doc.RootElement;
            if (!GetBool(root, "ok"))
            {
                error = GetString(root, "error");
                if (error.Length == 0) error = "сервер отказал";
                return false;
            }
            if (!root.TryGetProperty("controls", out var arr) || arr.ValueKind != JsonValueKind.Array)
            {
                error = "в ответе нет списка ручек";
                return false;
            }
            foreach (var el in arr.EnumerateArray())
            {
                if (el.ValueKind != JsonValueKind.Object) continue;
                controls.Add(new CameraControlInfo
                {
                    Domain = GetString(el, "domain"),
                    Id = GetLong(el, "id"),
                    Name = GetString(el, "name"),
                    Min = GetLong(el, "min"),
                    Max = GetLong(el, "max"),
                    Step = GetLong(el, "step", 1),
                    Def = GetLong(el, "def"),
                    Caps = GetLong(el, "caps"),
                    Cur = GetLong(el, "cur"),
                    Flags = GetLong(el, "flags", 2),
                    Supported = GetBool(el, "supported"),
                });
            }
        }
        return true;
    }

    // Разбор ответа get/set: ok → cur/flags.
    public static bool TryParseValueResponse(string response, out long cur, out long flags, out string error)
    {
        cur = 0; flags = 2; error = "";
        JsonDocument doc;
        try { doc = JsonDocument.Parse(response); }
        catch (Exception) { error = "ответ не похож на JSON"; return false; }
        using (doc)
        {
            var root = doc.RootElement;
            if (!GetBool(root, "ok"))
            {
                error = GetString(root, "error");
                if (error.Length == 0) error = "сервер отказал";
                return false;
            }
            cur = GetLong(root, "cur");
            flags = GetLong(root, "flags", 2);
        }
        return true;
    }

    private static bool GetBool(JsonElement el, string name) =>
        el.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.True;

    private static string GetString(JsonElement el, string name) =>
        el.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String
            ? v.GetString() ?? ""
            : "";

    private static long GetLong(JsonElement el, string name, long def = 0)
    {
        if (el.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number
            && v.TryGetInt64(out var n))
            return n;
        return def;
    }
}

// Тонкий клиент: одно соединение на запрос (как ControlClientRequest в C++).
// Все методы возвращают false + русский текст вместо исключений.
public sealed class CameraPipeClient
{
    private const int ConnectTimeoutMs = 800;
    private const int IoTimeoutMs = 1500;

    // Test-seam (прецедент: Settings.Load/Save(filePath)): прод holdings
    // используют имя по умолчанию; тесты поднимают canned-сервер на своём
    // имени и не гоняются с живым хостом за один pipe.
    public string PipeName { get; set; } = CameraControlProtocol.PipeName;

    public bool TryList(out List<CameraControlInfo> controls, out string error)
    {
        controls = new List<CameraControlInfo>();
        error = "";
        if (!TryRequest(CameraControlProtocol.BuildListRequest(), out var response, out error))
            return false;
        return CameraControlProtocol.TryParseList(response, out controls, out error);
    }

    public bool TryGet(string domain, string name, out long cur, out long flags, out string error)
    {
        cur = 0; flags = 2; error = "";
        if (!TryRequest(CameraControlProtocol.BuildGetRequest(domain, name), out var response, out error))
            return false;
        return CameraControlProtocol.TryParseValueResponse(response, out cur, out flags, out error);
    }

    public bool TrySet(string domain, string name, long value, long flags,
        out long applied, out long appliedFlags, out string error)
    {
        applied = 0; appliedFlags = 2; error = "";
        if (!TryRequest(CameraControlProtocol.BuildSetRequest(domain, name, value, flags), out var response, out error))
            return false;
        return CameraControlProtocol.TryParseValueResponse(response, out applied, out appliedFlags, out error);
    }

    private bool TryRequest(string requestJson, out string response, out string error)
    {
        response = "";
        error = "";
        var pipeName = PipeName;
        try
        {
            // Overlapped-хэндл: асинхронное чтение для дедлайна ниже.
            using var pipe = new NamedPipeClientStream(
                ".", pipeName, PipeDirection.InOut, PipeOptions.Asynchronous);
            try
            {
                pipe.Connect(ConnectTimeoutMs);
            }
            catch (TimeoutException)
            {
                error = NoServerText();
                return false;
            }
            catch (IOException)
            {
                error = NoServerText();
                return false;
            }
            pipe.ReadMode = PipeTransmissionMode.Byte;

            var payload = Encoding.UTF8.GetBytes(requestJson + "\n");
            pipe.Write(payload, 0, payload.Length);
            pipe.Flush();

            // Чтение с дедлайном через ReadAsync+Wait: у NamedPipeClientStream
            // нет поддержки ReadTimeout/WriteTimeout (InvalidOperation).
            var acc = new StringBuilder();
            var buf = new byte[4096];
            var deadline = Environment.TickCount64 + IoTimeoutMs;
            for (;;)
            {
                var left = deadline - Environment.TickCount64;
                if (left <= 0)
                {
                    error = "сервер управления не ответил вовремя";
                    return false;
                }
                var readTask = pipe.ReadAsync(buf, 0, buf.Length);
                bool done;
                try
                {
                    done = readTask.Wait((int)Math.Min(left, int.MaxValue));
                }
                catch (AggregateException ae) when (ae.InnerException is IOException)
                {
                    error = "сервер управления закрыл соединение";
                    return false;
                }
                if (!done)
                {
                    error = "сервер управления не ответил вовремя";
                    return false;
                }
                int read = readTask.Result;
                if (read <= 0)
                {
                    error = "сервер управления закрыл соединение";
                    return false;
                }
                acc.Append(Encoding.UTF8.GetString(buf, 0, read));
                var s = acc.ToString();
                var nl = s.IndexOf('\n');
                if (nl >= 0)
                {
                    response = s[..nl].TrimEnd('\r');
                    return true;
                }
                if (acc.Length > 256 * 1024)
                {
                    error = "ответ сервера слишком длинный";
                    return false;
                }
            }
        }
        catch (Exception ex)
        {
            error = $"неожиданная ошибка связи: {ex.Message}";
            return false;
        }
    }

    private static string NoServerText() =>
        "сервер управления недоступен — переключите эфир на камеру и дождитесь, пока хост её откроет";
}
