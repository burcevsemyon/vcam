---
name: vcam-e2e
description: >-
  E2E-тесты VCam: e2e_test.ps1 (фазы A/B VCamProducerCli + CaptureTest), бэкап/restore
  settings.json, конфликт двух писателей, тестовые медиа e2e_output, assert движения/статики
  по BMP-хэшам. Use when: e2e_test, VCam e2e, CaptureTest, проверка виртуальной камеры,
  hot-switch тест, «два писателя в секцию», кадры не идут/идентичны.
---

# VCam E2E (проектный skill)

Пайплайн теста: **продьюсер** (`VCamProducerCli run`) → shared memory
`Global\VCam.FrameBuffer.v1` → **камера** (`MediaSource.dll`) → **захват**
(`CaptureTest.exe` → BMP) → **ассерты** (nonBlack + SHA256 хэши кадров).

Подробный скрипт: `VirtualCameraMediaSource/e2e_test.ps1`.
Память задач: `VirtualCameraMediaSource/vcam-*.memory.md` (читать память
вышестоящей задачи перед работой).

## Перед прогоном (обязательно)

1. **Остановить tray-хост** `VCamVideoStreamProducer` (и CLI) — два писателя
   в одну секцию интерливят кадры. Скрипт лишь печатает WARNING и НЕ падает —
   проверка обязана быть до запуска: `Stop-Process -Name VCamVideoStreamProducer`.
2. Камера зарегистрирована: `build\x64\Release\Registrar.exe add VCam hold`
   (elevated; процесс не закрывать). Скрипт дополнительно делает
   `regsvr32 /s MediaSource.dll`. Смены камеры видны после перезапуска
   страницы «Камеры» в Windows Settings.
3. Тестовые медиа: `e2e_output\test_video.mp4` (5 с, testsrc2 320x240;
   `test_video2.mp4`, `pattern_top_white.mp4` — для hot-reload/ориентации).
   `test_input.bmp` нет — скрипт скопирует из
   `C:\Users\Semen\source\repos\10_000.bmp`.
4. MSB3027 — закрыть свои exe перед сборкой (скрипт сам собирает solution).

## Запуск

```powershell
cd VirtualCameraMediaSource
powershell -ExecutionPolicy Bypass -File e2e_test.ps1   # exit 0 = SUCCESS
```

- **Фаза A** — `run --type video --path <test_video.mp4>` (overrides):
  кадры должны ДВИГАТЬСЯ (unique SHA256 > 1), в логе `writer ready` и
  `[cli] active:`.
- **Фаза B** — `run` БЕЗ overrides + перезапись settings.json:
  static → кадры идентичны (unique = 1) → video → движутся; в логе
  ≥2 × `[cli] switch:` и `[cli] source opened: type=video` (проверка
  hot-switch без перезапуска, через SettingsWatcher).
- **finally** — settings.json восстанавливается **байт-в-байт**
  (SHA256-сверка); если файла не было — тестовый удаляется. Нарушение
  SHA256 = FAIL.
- Артефакты: `e2e_output\` (BMP `prefix_*.bmp`, `cli_phase_a/b.log`,
  `cli_phase_a/b.err`, `capture_*.log`).

## Ассерты (как считаются)

- **nonBlack**: сэмпл BMP каждые 32×18 px, значения >10 по любому каналу;
  max > 0 — значит продьюсер пишет (0 = чёрный экран = кадров нет).
- **Движение/статика**: SHA256 каждого кадра; video → уникальных > 1,
  static → ровно 1.
- CaptureTest direct: `CaptureTest.exe <n> <prefix>` → BMP 1280×720 24-бит
  (2 764 854 байт). Лог захвата: `e2e_output\capture_<prefix>.log`.

## Питфолы (это ломалось на практике)

- `%APPDATA%` внутри printf-строки C++ → invalid parameter handler →
  exit `0xC0000409`. Только `%%APPDATA%%`.
- JSON-ключ матчить **только с `:` после него** (баг FindKeyPos): иначе
  `"type":"video"` (значение) ловится как имя секции → читается static.
  Симптом: type=video, а кадры идентичны / `status` показывает video.path
  от static.
- PowerShell 5.1: `Set-Content -Encoding UTF8` пишет **BOM**. Для settings:
  `[IO.File]::WriteAllText($path,$json,(New-Object System.Text.UTF8Encoding($false)))`.
- Overrides `--type/--path` фиксируют источник на весь запуск и игнорируют
  правки settings.json — честный hot-switch тестируется только без
  overrides (фаза B).
- CLI из скрипта: `Start-Process -PassThru -RedirectStandard*`, остановка —
  `Stop-Process` + `WaitForExit`; ожидание кадров — `Start-Sleep 4 c`
  перед каждой проверкой (старт writer ≈ 1 с, первый кадр — сразу после).
- Единственный writer на время теста = CLI; после — убедиться, что процесс
  убит (`finally` в скрипте это делает, но при ручных запусках — проверить).
- UI round-trip тестов постоянного проекта НЕТ: воссоздаются временным
  net10.0-windows проектом в `%TEMP%` (см. `vcam-video-ui.memory.md`,
  §«Воспроизведение тестов»); тестовый файл
  `%TEMP%\vcam-ui-roundtrip-data\settings.json`, рабочий
  `%APPDATA%\VCam\settings.json` не трогать.
- Быстрая живая диагностика без скрипта:
  - `VCamProducerCli.exe status` — seq растёт = пишется (status сам не пишет);
  - скриншот окна `VCamPreview` + mean яркость (640×360): статика ≈ 48,
    NO SIGNAL ≈ 15 — надёжно отличает фазы;
  - остановка хоста: named event `VCamVideoStreamProducer.Stop` → `Set()`
    (так же, как кнопка в UI);
  - stop/start: после рестарта хоста превью возвращается ≤ ~4 с
    (окно реконнекта 3 с + retry 1 с).

## Связанные константы контракта

- 1280×720 BGRX, stride 5120, 8 слотов, seqlock; fallback: нет свежего
  кадра 40 мс → кэш последнего кадра (не чёрный); без кэша → NO SIGNAL.
- Тайминги хоста: hot-switch окно 5 с (FlushLast), Preview: timeout 1.5 с,
  reconnect 3 с.
