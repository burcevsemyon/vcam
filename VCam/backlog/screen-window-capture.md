# Трансляция экрана / окна как источник

## Идея

Добавить захват экрана (монитор) или отдельного окна как тип источника
виртуальной камеры (`screen` / `window`) — трансляция рабочего стола или
конкретного приложения вместо статики/видео/камеры.

## Мотивация

Сейчас источники — статичное изображение, видеофайл или камера. Показать
экран/окно без VCam можно только сторонними OBS-подобными микшерами.
Захват экрана — один из самых частых сценариев использования виртуальной
камеры (демонстрация, обучение, презентации).

## Возможная реализация

- Новый тип источника в `settings.json` (`source.type: "screen"|"window"`,
  секция `screen: {monitorIndex | windowTitle, showCursor, showBorder}`)
  + сиды/миграция по образцу существующих секций.
- Реализация захвата — Windows Graphics Capture API (`IGraphicsCaptureItem`,
  WinRT, Windows 10 1903+): быстрый (DXGI-based), ловит окна/мониторы,
  корректно работает с оверлеями и DPI. Fallback для старых систем —
  GDI BitBlt рабочего стола (только монитор, проще и без WinRT).
  Возможно и Desktop Duplication API (IDXGIOutputDuplication) как
  альтернативу Graphics Capture.
- Новый `ScreenFrameSource` в `ProducerCore` по контракту `IFrameSource`
  (как `StaticImageSource`/`VideoFileSource`), регистрация в `SourceFactory`;
  такт источника — в рамках общего pacing `FrameWriter` (30 FPS), чтобы
  не ломать единственного писателя shared-memory.
- Выбор окна/монитора: UI-список в `VCamSettingsUi` (EnumWindows → список
  видимых окон, EnumDisplayMonitors → мониторы), в CLI — `list-sources`
  аналогично `list-devices`.
- Хоткей быстрого вкл/выкл захвата (секция `screenHotkey`) — по образцу
  `sourceStaticHotkey` и т.п.
- Производительность: Graphics Capture даёт ~GPU-путь; читать кадр только
  при изменении (dirty rect / frame-arrived event), не дёргать каждые 33 мс
  без изменений. Учесть DPI-scaling и per-monitor v2.
- Границы приватности: окно захвата выбирает пользователь; не добавлять
  скрытых «захватов всего» по умолчанию — явное включение.

## Приоритет

Low

## Статус

Идея
