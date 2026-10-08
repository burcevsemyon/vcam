# Переключение источника из трея (context menu)

## Идея

Добавить в контекстное меню иконки в трее пункты для быстрого переключения
источника (static / video / camera) без запуска UI.

## Мотивация

Сейчас для смены источника нужно открыть VCamSettingsUi, выбрать источник
и применить. Для частого переключения (например, между статичным изображением
и видео) это лишний шаг.

## Возможная реализация

- В `VCamVideoStreamProducer` (tray-хост) добавить `ContextMenuStrip`
  с подменю «Источник» → «Static», «Video», «Camera».
- При выборе — записать новое значение в `settings.json` (секция `source.type`),
  `SettingsWatcher` подхватит изменение и выполнит hot-switch.
- Отметить текущий источник checkmark'ом.
- Горячие клавиши: глобальные хоткеи (например, `Ctrl+Alt+1` → static,
  `Ctrl+Alt+2` → video, `Ctrl+Alt+3` → camera) через `RegisterHotKey` в хосте.
  Конфигурация в `settings.json` (секция `hotkeys.sourceSwitch`).

## Приоритет

Low

## Статус

**Сделано (08.10.2026):**
- Подменю «Источник» в трее (Static/Video/Camera) + `ApplySourceSwitch`
  (`ProducerCore/TraySourceMenu.*`).
- Глобальные хоткеи переключения источника (секции `sourceStaticHotkey` /
  `sourceVideoHotkey` / `sourceCameraHotkey`, дефолты Ctrl+Alt+1/2/3):
  `RegisterHotKey` в `VCamVideoStreamProducer` (id 4/5/6), диспатч на
  `SwitchSource(type, origin)`, hot-reload через `ApplySettingsDiff`,
  редакторы в `VCamSettingsUi` (группа «Хоткеи источников»).
  Юниты: `test_hotkey`/`test_settings`/`test_sections`/`test_framecopy_layout`
  (C++) и `SettingsSourceHotkeyTests` (C#).
