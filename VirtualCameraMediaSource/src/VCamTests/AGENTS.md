# VCamTests — питфоллы тестового проекта

doctest-юниты для VCam; общий контекст — корневой `AGENTS.md` в корне репо.

- **Собирать только через sln** (`VirtualCameraMediaSource.sln`, Release|x64).
  Сборка `VCamTests.vcxproj` напрямую даёт exe в другом каталоге (`src\VCamTests\build\`)
  — легко запустить устаревший бинарь и получить ложнозелёный прогон.
- Запуск: `build\x64\Release\VCamTests.exe`; фильтр по имени кейса: `-tc="имя*"`.
  Exit 0 = SUCCESS. Warning `C5285` из `doctest.h` — норма.
- Новый тест = `test_*.cpp` + строка `<ClCompile>` в `VCamTests.vcxproj`.
  Из ProducerCore линкуются только явно перечисленные в vcxproj `.cpp` —
  новый источник добавлять туда же.
- Генерация тестовых изображений (WIC) — образец в `test_static_image.cpp`.
