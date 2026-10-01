# VCam

**Виртуальная камера для Windows** — подставляет вашу картинку, видео или
физическую веб-камеру как обычное камерное устройство для любых приложений:
Zoom, ktalk, Discord, «Камеры» Windows и других.

- **Три источника** — статичное изображение, видеоролик (зацикленно) или
  трансляция с реальной веб-камеры
- **Смена источника на лету** — без перезапуска, при ошибках камера
  показывает последний кадр или «NO SIGNAL»
- **Удобное управление** — окно настроек с предпросмотром и обрезкой,
  плавающее окно предпросмотра, меню в трее
- **Автозапуск** — камера готова сразу после входа в Windows
- **Готовый установщик** — установка в один клик

## Установка

Скачайте последний `VCamSetup-*-x64.exe` из [`releases/`](releases/)
и запустите (или `/VERYSILENT` для тихой установки). После установки камера
сразу доступна в системе.

Удаление — через «Приложения и возможности», пользовательские настройки
сохраняются.

## Использование

1. В трее появится иконка VCam — в меню выберите **«Настройки VCam…»**.
2. Укажите источник: картинку, видеоролик или физическую камеру.
3. Выберите **VCam** в качестве камеры в нужном приложении.

Смена источника применяется мгновенно, без перезапуска. Если продьюсер не
запущен — приложение покажет чёрный экран/«нет сигнала» (штатное состояние).

Консольный режим для отладки:

```powershell
VCamProducerCli.exe run                 # источник из settings.json
VCamProducerCli.exe run --type video --path clip.mp4
VCamProducerCli.exe list-devices        # список физических камер
VCamProducerCli.exe status              # состояние
```

## Сборка из исходников

Требуется Visual Studio (C++ и .NET) и x64:

```bat
msbuild VirtualCameraMediaSource\VirtualCameraMediaSource.sln /p:Configuration=Release /p:Platform=x64 /m
```

Результат — в `VirtualCameraMediaSource\build\x64\Release\`. Подробности
деплоя и запуска — в [README проекта](VirtualCameraMediaSource/README.md).

## Тесты

```powershell
powershell -ExecutionPolicy Bypass -File VirtualCameraMediaSource\e2e_test.ps1
```

Автоматический E2E-прогон всех режимов (exit code 0 = успех).

## Структура репозитория

| Путь | Что внутри |
|---|---|
| [`VirtualCameraMediaSource/`](VirtualCameraMediaSource/) | Проект: исходники, solution, e2e-тест, скрипты установщика, [подробный README](VirtualCameraMediaSource/README.md) |
| [`releases/`](releases/) | Готовые сборки установщика |
| [`.agents/skills/`](.agents/skills/) | Навыки AI-агентов для разработки и тестирования проекта |
| [`AGENTS.md`](AGENTS.md) | Инструкции для AI-агентов |

## Требования

Windows 11 (x64). Для сборки — Visual Studio, для установщика — Inno Setup.
