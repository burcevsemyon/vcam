# vcam-producer-previewfix (subagent) — GDI+ масштабирование VCamPreview + single-instance

Задача: A) рендер кадра через GDI+ HighQualityBicubic вместо StretchDIBits (муар при 0.5×); B) single-instance для VCamPreview (C++) и VCamSettingsUi (C#).
Ветка: feature/vcam-video-stream-producer. Память вышестоящей: `vcam-producer.memory.md`. Лимит: 2 раунда.
**Статус: DONE (1 раунд, без фиксов).**

## Изменённые файлы (2, не коммичено)
- `src/VCamPreview/VCamPreview.cpp`
  - `#include <gdiplus.h>` + `#pragma comment(lib, "gdiplus.lib")` (vcxproj не менялся — линковка через pragma).
  - `InitGdiplus()/ShutdownGdiplus()` (GdiplusStartupInput, токен `gGdiplusToken`), вызовы в wWinMain (до RegisterClass / после message loop, во всех ветках ошибок).
  - `PaintFrame`: `Gdiplus::Bitmap(frameW, frameH, frameW*4, PixelFormat32bppRGB, g.pFrame)` **без копирования** (буфер верхне-лежачий BGRX) + `Graphics(memDC)` → SetInterpolationMode(HighQualityBicubic), SetPixelOffsetMode(Half), SetCompositingQuality(HighQuality), SetCompositingMode(SourceCopy), DrawImage в Rect(dx,dy,dw,dh). Та же letterbox-геометрия (scale/dx/dy), что и было; PatBlt BLACKNESS и BitBlt в окно — без изменений; PaintNoSignal — без изменений.
  - Без копирования безопасно: весь рендер (Tick→ReadFrame и OnPaint) идёт в одном UI-потоке через WM_TIMER/WM_PAINT, других потоков в приложении нет.
  - Single-instance: `CreateMutexW(nullptr, TRUE, L"VCamPreview.Instance")` до создания окна; `ERROR_ALREADY_EXISTS` → `FindWindowW(L"VCamPreviewClass", nullptr)` → (если iconic) `SendNotifyMessage WM_SYSCOMMAND SC_RESTORE` + `ShowWindow(SW_RESTORE)` + `BringWindowToTop` + `SetForegroundWindow` (с AttachThreadInput к foreground-потоку; fallback — SetWindowPos HWND_TOPMOST|SWP_SHOWWINDOW) → `CloseHandle` → `return 0` (окно не создаётся). Первый экземпляр: мьютекс до выхода (ReleaseMutex+CloseHandle после цикла сообщений).
- `src/VCamSettingsUi/Program.cs`
  - `Mutex(true, "VCamSettingsUi.Instance", out createdNew)` в Main; `!createdNew` → `ActivateExistingInstance()` (`Process.GetProcessesByName("VCamSettingsUi")`, MainWindowHandle ≠ 0, IsIconic→ShowWindow(SW_RESTORE), SetForegroundWindow) → `Environment.Exit(0)` до создания формы.
  - Основной экземпляр: try/finally `ReleaseMutex()`; `using`-dispose. Своё окно в enumeration отсекается нулевым MainWindowHandle.

## Приёмка (факты)
1. **Сборка**: `dotnet restore` (UI) → MSBuild Release|x64 → **exit 0**, 1 раунд (0 ошибок компиляции).
2. **Качество**: скриншот окна (640×360 client, 656×399 window) `Temp\opencode\croptest\preview_new.png` — картинка гладкая, без муара/сетки, полосы чёрные; контент = эталон `raw_static.png` (тот же поляроид). Численно (клиентская область vs эталон, усреднение по небл. пикселям):
   - **MAE vs GDI+ HighQualityBicubic = 0.023** (рендер совпадает с эталонной бикубической масштабацией);
   - **MAE vs Nearest (эмуляция COLORONCOLOR) = 7.174**;
   - энергия высоких частот HF(|dx|): **shot = 15.584**, bicubic-ref = 15.587, nearest-ref = 16.905 → наш рендер на уровне бикубика, nearest даёт +8.5% алиасинга.
   - Скриншот после ресайзов (произвольный масштаб 624×321 → 0.4875×) `preview_after_resize.png` — тоже гладкий.
3. **Preview single-instance**: 2 последовательных запуска → `secondExit=0`, `Get-Process VCamPreview = 1` (pid 15704), окон с классом VCamPreviewClass: 1 → 1 (второе окно не появилось). Доп. проверка активации: окно свёрнуто (SW_MINIMIZE) → второй запуск → `iconic=False, visible=True` → FindWindow внутри приложения нашёл окно и развернул его.
4. **UI single-instance**: 2 запуска → `secondExit=0`, `Get-Process VCamSettingsUi = 1` (pid 28780); то же свернуть→второй запуск→`iconic=False`. UI после проверок закрыт.
5. **Тесты UI**: `%TEMP%\opencode\ui_settings_test\settingsTest.csproj` → **PASS=25 FAIL=0**, `ALL TESTS PASSED`, exit 0.

## Доп. замеры (не просили, но полезно)
- GDI-объекты: 17→17 за 8 с (~240 кадров); после 6 циклов ресайза 18→17. USER: 16→16. **Утечек GDI нет.**
- CPU видимого окна: **~330 мс/с = ~11 мс/кадр @30 FPS** (бюджет 4 мс превышен в ~2.7×, но 11 < 33 мс — кадры не срываются); при свёрнутом окне ~0.

## Питфолл (запомнить)
- **PowerShell P/Invoke: `FindWindowW(cls, $null)` → передаёт "" а не NULL** и по классу ничего не находит. Работает только реальный NULL (`IntPtr.Zero` через перегрузку с IntPtr). На C++ `FindWindowW(cls, nullptr)` — штатный NULL, работает (проверено IntPtr-обёрткой: находит). В тестовых скриптах — только `StringToHGlobalUni` + `IntPtr.Zero`.
- Внешняя команда Bash идёт через PowerShell → `$var` в двойных кавычках интерполируется внешним шеллом; inline `-Command` с кавычками ненадёжен — писать .ps1.
- `Add-Type` с System.Drawing требует `-ReferencedAssemblies System.Drawing` и `using System.Drawing.Imaging`.
- `Bitmap.LockBits` дважды на одном объекте → InvalidOperationException (сравнение картинки с самой собой).

## Риски
- **Производительность ~11 мс/кадр** (GDI+ бикубик по 1280×720) — в 2.7× выше ориентира 4 мс; по условию приоритет — качество, кадры не срываются. Если понадобится ускорить: кэшировать Bitmap/Graphics между кадрами (пересоздание сейчас каждый кадр) или ограничить перерисовку сменой seq.
- Активация `SetForegroundWindow` из второго экземпляра может блокироваться foreground-политикой Win10/11 (как в tray-задаче) — восстановление из свёрнутого проверено, «подъём в foreground» управляется ОС.
- NO SIGNAL (`PaintNoSignal`) вживую не проверялся — код не менялся (требует остановки хоста, хост не трогаем).
- `e2e_test.ps1` не прогонялся (в приёмке не требуется); CaptureTest/e2e не тронуты.
- Заголовок окна с mojibake («вЪ» вместо «—») — известный баг этапа1, вне scope.
- Среда: VCamPreview (pid 15704) оставлен запущенным (как до задачи), VCamSettingsUi закрыт, хост VCamVideoStreamProducer (22064) не трогался.
