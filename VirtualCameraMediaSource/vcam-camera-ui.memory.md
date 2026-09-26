# vcam-camera-ui (subagent C — этап C, UI) — SourceType.Camera + панель выбора камеры

Задача: этап C плана `vcam-camera.memory.md` — ТОЛЬКО C# UI (`src/VCamSettingsUi`:
Settings.cs, MainForm.cs) + расширение round-trip тестов. Ветка: feature/vcam-camera-source.
Память вышестоящей: vcam-camera.memory.md + vcam-camera-cli.memory.md (контракт list-devices) +
vcam-camera-core.memory.md (секция camera в C++) + vcam-producer*.memory.md.
Лимит 2 раунда. Коммит НЕ делался.

## Статус
- **DONE — 1 раунд** (все прогоны с первого раза: тесты 43/43, сборка exit 0, UIA-смоук FAILS=0).

## Изменения

### `src/VCamSettingsUi/Settings.cs`
- `SourceType.Camera` (третий член enum).
- Новые свойства `CameraId`, `CameraName` (секция `camera`; оба "" = камера не выбрана → NO SIGNAL).
- Load: `isNew` детект += ключ `"camera"`; `source.type == "camera"` (OrdinalIgnoreCase) →
  `SourceType.Camera` (иначе, как раньше: video/Static); чтение секции `camera` (`id`, `name`).
- Save: `"type": SourceType switch { Video=>"video", Camera=>"camera", _=>"static" }`;
  секция `"camera": { "id": ..., "name": ... }` пишется ВСЕГДА (зеркально C++ `Serialize`,
  который пишет её перед `autostart`); legacy-ветка Load не менялась.
- XML/комментарий-контракт над классом обновлён (type union + camera-секция + пустая секция → NO SIGNAL).

### `src/VCamSettingsUi/MainForm.cs`
- `MediaNames` += `"физическая камера"` (индекс 2); `CurrentSourceType` — switch по индексу
  (0 Static / 1 Video / 2 Camera).
- Новое: `_cameraPanel` (белый, FixedSingle, 856×455 на месте превью, Name=`cameraPanel`),
  внутри: title «Физическая камера», `_cameraDevLabel` «Камера:», `_cameraCombo`
  (DropDownList, Name=`cameraCombo`), `_cameraRefresh` «Обновить список» (Name=`cameraRefresh`),
  `_cameraIdLabel` (ID выбранного / NO SIGNAL), `_cameraHint` (как выбирать, NO SIGNAL при
  отсутствии/занятости, «(недоступно)»), `_cameraStatus` (CLI найден/сколько камер).
- `UpdateLayout()`: `camera` ветка (по образцу static↔video) — `_cameraPanel.Visible=camera`,
  скрыты `_preview`/`_cropView`/`_mode`/`_fullSizeButton`/crop-поля/`_openButton`/`_pathLabel`.
- **Перечисление камер — `VCamProducerCli list-devices`** (лениво: `EnsureCameraList()` при первом
  показе панели, т.е. при первом переключении НА camera; если сохранённый type=camera — при
  старте UI в ctor; кнопка «Обновить список» — повторный прогон):
  `ProcessStartInfo { Arguments="list-devices", UseShellExecute=false, Redirect stdout+stderr,
  StandardOutputEncoding/StandardErrorEncoding=UTF8, CreateNoWindow=true }`; stdout и stderr
  читаются асинхронными Task'ами (анти-deadlock), `WaitForExit(8000)` → таймаут = `Kill()` СВОЕГО
  дочернего + статус; парсинг ТОЛЬКО stdout: строки `id\tname` (split '\n', TrimEnd '\r',
  первый tab, id non-empty). Путь CLI — `FindCliExe()` по образцу `_previewExe`:
  (а) рядом с `VCamSettingsUi.exe`, (б) вверх до 10 предков + `build\x64\Release\VCamProducerCli.exe`.
  Не найден / ошибка запуска → список пуст + текст в `_cameraStatus`, без крашей.
- Сохранённый выбор: `_cameraWishId/_cameraWishName` из settings (выставляются в
  `LoadCurrentSettings` ДО индекса `_mediaCombo`); `FillCameraCombo()` после каждой загрузки:
  выбор по id (OrdinalIgnoreCase) → по name → иначе, если id не пуст, добавляется
  `CameraItem(id, name, missing:true)` → ToString = `"<name> (недоступно)"` (id/name не теряются);
  ничего не выбрано → SelectedItem=null.
- `CameraItem` — приватный class (primary ctor): `Id/Name/Missing`, ToString = отображение.
- Save (ветка camera в `OnSaveClicked`): из выбранного элемента `CameraId`+`CameraName`
  (для missing — исходное Name без «(недоступно)»), `SourceType=Camera`; **без выбора сохранять
  разрешено** — `TrySave(..., warn:true)` → `ForeColor=DarkGoldenrod` + текст про NO SIGNAL
  в `_hintLabel` (успех — ForestGreen, как раньше).

### Тесты `%TEMP%\opencode\ui_settings_test\Program.cs` (25 → 43)
- `Equal()` += `CameraId`/`CameraName`.
- Новые блоки 7-11: camera round-trip (type/id/name, нет BOM, нет другого type-токена,
  autostart=false); type=camera БЕЗ секции camera (→ Camera + пустые поля + Save пишет пустую
  секцию); legacy json → тип не camera + Save пишет пустую секцию и не "camera";
  `source.type=camera` → Camera в UI + Save пишет "camera"; спецсимволы/кириллица/кавычки в id+name.

## Приёмка (факты)
1. `dotnet restore` (VCamSettingsUi.csproj) → ok; MSBuild `VirtualCameraMediaSource.sln
   /p:Configuration=Release /p:Platform=x64 /m` → **exit 0** (1 прогон).
2. Тесты: `dotnet run -c Release --project %TEMP%\opencode\ui_settings_test\settingsTest.csproj`
   → **43/43 PASS, ALL TESTS PASSED, exit 0** (25 старых + 18 camera).
3. UIA-смоук (`%TEMP%\opencode\vcam_camera_smoke.ps1`, один прогон) → **FAILS=0**: окно поднялось;
   mediaCombo = «статичная картинка | видеоролик | физическая камера» (3 пункта);
   выбор индекса 2 → cameraPanel виден; `_cameraStatus` = «Камер найдено: 1.»;
   cameraCombo = «Brio 90» (единственная), выбран; cameraIdLabel = `ID: \\?\usb#vid_046d…\global`;
   cameraRefresh найден; Save (Invoke) → окно закрыто, процесс завершён (остатков нет).
4. `%APPDATA%\VCam\settings.json` ПОСЛЕ Save: 566 B, первые байты 123,13,10 (без BOM),
   `"source":{"type":"camera"}`, секция `camera` c id=`\\?\usb#vid_046d&pid_0949…\global`
   и name=`Brio 90`, static/video/autostart не тронуты.
   **ВОССТАНОВЛЕН байт-в-байт из бэкапа** `%TEMP%\opencode\vcam_camera_smoke.bak`:
   SHA256 до/после = `317B61146E8A82B898EDBA81B9C289D93FC75368D3BB45343D149B6F211F61FA` — идентично
   (до смоука файл был 409 B, type=static). Смоук-скрипт выжил в temp для повторов.
5. `git status`: M MainForm.cs, M Settings.cs — мои; остальные M/?? — этапы A/B (не тронуты);
   ветка feature/vcam-camera-source. Коммит не делался.

## Решения (описание поведения)
- **Ленивая загрузка списка**: при первом показе `_cameraPanel` (первый выбор camera, включая
  запуск с сохранённым type=camera — тогда в ctor). При type=static/video CLI НЕ запускается.
  «Обновить список» — всегда повторный прогон (сохранение текущего/запомненного выбора).
- UIA в смоуке выбирал элемент по ИНДЕКСУ (ascii-only скрипт); имя пункта подтверждено косвенно
  (cameraPanel + статус «Камер найдено: 1»).

## Риски
- Загрузка списка синхронна на UI-потоке (WaitForExit до 8 с) — на практике ~0.3 с; при зависшем
  CLI окно «замрёт» до таймаута (статус объяснит).
- `cameraIdLabel` показывает полный MF-symlink (длинный; AutoEllipsis обрезает) — осознанно
  (метка id нужна для диагностики).
- Смоук Save переключал хост на камеру и обратно (hot-reload ~1 с) — штатно, файл восстановлен.
- Нет проверки Save через живой клик по кнопке с открытым dropdown — Invoke по saveButton (паттерн UIA).

## Запрос
Не требуется. Этап C закрыт; следующий — основной (README/хост-логи/финальная сборка+e2e).
