# vcam-producer-tray (subagent 2) — хост VCamVideoStreamProducer + UI

Задача: этап 2/3 — Win32 tray-хост (single-instance, Stop event, hot-switch, autostart HKCU Run) + правки VCamSettingsUi (новая схема settings, кнопка запуска/остановки хоста).
Ветка: feature/vcam-video-stream-producer. Память: vcam-producer.memory.md (утверждённый дизайн) + vcam-producer-core.memory.md (ProducerCore API) + vcam-video.memory.md (MSBuild/E2E/контракт).
Лимит: 2 раунда (раунд = сборка→правка→пересборка/перетест).

## Статус
- [x] Раунд 1: код (host, ProducerCore-фикс, UI) → сборка exit 0 (2 ошибки WAIT_OBJECT_1/WM_LBUTTONDBLAP → WAIT_OBJECT_0+1 / WM_LBUTTONDBLCLK).
- [x] Приёмка 1-7: все выполнены (см. факты).

## Факты
- П1 (сборка): MSBuild Release/x64 exit 0; повторно подтверждено полной пересборкой в e2e_test.ps1.
- П2 (хост): старта/логи в `...\vcam_tray_test\host1|host2.out.log` (starting/autostart/tray icon added/watching/writer ready/switch/active/no signal/exit); single-instance — второй экземпляр логнул «already running» + MessageBox, завершён; останов по Stop-event подтверждена; окно класса `VCamVideoStreamProducerWnd` (enum); автозапуск HKCU Run\VCamAutostart создан хостом (default true, штатно). Tray-меню: открытие подтверждено (popup #32768 у процесса хоста), диспетчеризация подтверждена логом `exit requested (tray menu)`; визуальный скриншот пунктов меню не снят — SetForegroundWindow к окну хоста из внешнего процесса блокируется foreground-политикой Win10/11 (тестовое окружение, не баг).
- П3 (hot-switch): seq-polling 50 мс по Global\VCam.FrameBuffer.v1 — static→video maxGap=91.8 мс, video→static maxGap=92.4 мс (<1200 ✓), сигнатура кадра меняется (sigChanges=10 video), скриншоты превью живые (hot_video.png ratio=0.753, hot_static.png 0.338); логи switch/source opened/active за оба перехода.
- П4 (fallback bogus): type=bogus → seq froze=3658.5 мс (>1500 ✓), лог `no signal: неизвестный тип источника: bogus`, скриншот NO SIGNAL (`bogus_nosignal_caught.png`); восстановление static → seq resumed (maxGap 78.9 мс), превью живое. Превью мерцает NO SIGNAL↔stale-кадр (1.5 с / 3 с) — штатная reconnect-логика VCamPreview (kReconnectAfterMs), этап 1.
- П5 (UI-кнопка): BM_CLICK цикл: «Хост: запущен»/«Остановить хост» → stop → «Хост: не запущен»/«Запустить хост», hostAlive=False → start → снова «запущен», hostAlive=True. Кодпоинты строк — чистый Cyrillic Unicode.
- П6 (round-trip): 25/25 PASS (`...\ui_settings_test\settingsTest.csproj`): legacy-миграция, нет BOM, стабильный Save, кириллица, autostart=false, unknown type→Static, defaults.
- П7 (e2e_test.ps1): exit 0, 5/5 кадров (пересборка решения внутри).
- Финиш: `%APPDATA%\VCam\settings.json` восстановлен из `...\Temp\opencode\settings.json.bak` (legacy-формат, байт-в-байт); своих процессов (host/UI/preview/producers) не осталось.

## Риски
- Tray-меню не отскриншочено визуально (foreground-политика) — контент виден только в коде ShowTrayMenu (VCamVideoStreamProducer.cpp:434); подтверждено косвенно (окно меню + лог команды).
- HKCU Run\VCamAutostart остался записанным (штатное поведение хоста, autostart=true по умолчанию) — при желании удалить вручную.
- Существующий баг этапа1 (не наш scope): строка статуса превью «— нет сигнала»/«— 1280x720@30» содержит mojibake (0x0432 0x0402 0x201D вместо «—») — двойное кодирование L-литералов в VCamPreview.cpp.
- Лог-вывод хоста с кириллицей отображается в cp-кракозябрах (кодировка sink'а), сами строки корректны.
- Settings.cpp: неизвестный type сохраняется и даёт пустой SourceConfig → CreateSource → nullptr → fallback (намеренный прод-фикс «bogus type», был в раунде 1).

## Запрос
- Нет: приёмка 1-7 закрыта, отчёт выдан, коммит по условию не делался.
