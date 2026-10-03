#pragma once

#include <string>

#include "ProducerApi.h"

struct StaticSection {
    std::wstring path;
    std::wstring scaleMode = L"fit"; // fit | cover | crop
    int cropX = 0;
    int cropY = 0;
    int cropW = 0;
    int cropH = 0;
    bool cropKeepAspect = false;

    bool operator==(const StaticSection& o) const
    {
        return path == o.path && scaleMode == o.scaleMode && cropX == o.cropX &&
               cropY == o.cropY && cropW == o.cropW && cropH == o.cropH &&
               cropKeepAspect == o.cropKeepAspect;
    }
};

struct VideoSection {
    std::wstring path;

    bool operator==(const VideoSection& o) const { return path == o.path; }
};

struct CameraSection {
    std::wstring id;   // MF symlink (MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID)
    std::wstring name; // friendly name
    // Разрешение захвата: L"max" (def, лучшее в cap — поведение quality-ветки) |
    // L"720p" | L"1080p" (предпочесть высоту; смена = переоткрытие через ==).
    std::wstring capture = L"max";

    bool operator==(const CameraSection& o) const
    {
        return id == o.id && name == o.name && capture == o.capture;
    }
};

// Глобальный хоткей хоста static→video→auto-static (секция hotkey).
// Модификаторы — биты RegisterHotKey (MOD_ALT=1, MOD_CONTROL=2, MOD_SHIFT=4,
// MOD_WIN=8); vk — Virtual-Key code. Мусор/отсутствие → дефолт Ctrl+Alt+V.
struct HotkeySection {
    int modifiers = 3; // MOD_CONTROL | MOD_ALT
    int vk = 0x56;     // 'V'

    bool operator==(const HotkeySection& o) const
    {
        return modifiers == o.modifiers && vk == o.vk;
    }
    bool operator!=(const HotkeySection& o) const { return !(*this == o); }
};

// Хоткей старт/стоп записи эфира (секция recordHotkey, рядом с hotkey).
// Та же семантика полей; мусор/отсутствие → дефолт Ctrl+Alt+R.
struct RecordHotkeySection {
    int modifiers = 3; // MOD_CONTROL | MOD_ALT
    int vk = 0x52;     // 'R'

    bool operator==(const RecordHotkeySection& o) const
    {
        return modifiers == o.modifiers && vk == o.vk;
    }
    bool operator!=(const RecordHotkeySection& o) const { return !(*this == o); }
};

// Путь по умолчанию для записи эфира (секция record). Пусто = хост
// сгенерирует %Videos%\VCam_ГГГГММДД_ЧЧММСС.mp4 при старте записи.
// Состояние записи (идёт/нет) здесь НЕ живёт — только transient
// %APPDATA%\VCam\record_state.json, иначе watcher зациклит старт/стоп.
struct RecordSection {
    std::wstring path;

    bool operator==(const RecordSection& o) const { return path == o.path; }
    bool operator!=(const RecordSection& o) const { return !(*this == o); }
};

struct EffectsSection {
    bool enabled = true;    // мастер-выключатель: false = хост скипает ApplyFx целиком
    bool mirror = false;    // зеркало по горизонтали
    bool grayscale = false; // Ч/Б (Rec.709 luma, исполнитель — GPUPixel)
    // Аналоговые помехи (исполнитель — CPU в GpuEffects.cpp, после GPU):
    bool noise = false;     // RGB/белый шум (±60/канал, анимирован по кадрам)
    bool scanlines = false; // чересстрочные линии (нечётные строки ×0.35)
    bool rgbSplit = false;  // хроматическая аберрация (R/B-разъезд по X)
    bool tracking = false;  // трекинг-глитч (сдвинутые полосы, движутся)
    bool vhs = false;       // VHS-пресет: все четыре помехи сразу (OR)
    // Третья тройка — только backend frei0r (CPU-аналогов нет; на cpu
    // пропускаются с one-shot хинтом хоста, в VHS не входят):
    bool gateweave = false; // дрожание плёнки (амплитуда сдвига кадра)
    bool glow = false;      // свечение светов (сила screen-blend)
    bool denoise = false;   // шумодав hqdn3d (сила сглаживания)
    // Степень помех 0–100 (индивидуальные уровни; VHS использует их же,
    // отдельного vhsLevel нет). Отсутствует в JSON → 100, кламп при чтении.
    // Уровень 0 при включённом тоггле ≈ эффект выключен.
    int noiseLevel = 100;      // амплитуда шума ±(level% от ±60)
    int scanlinesLevel = 100;  // глубина затемнения нечётных строк
    int rgbSplitLevel = 100;   // dx сдвига (100 → 6/12/24 по ширине)
    int trackingLevel = 100;   // число/ширина полос (0 → нет полос)
    int gateweaveLevel = 100;  // размах дрожания (100 → ±10px)
    int glowLevel = 100;       // радиус/сила свечения (100 → kernel ~32px@1280)
    int denoiseLevel = 100;    // сила шумодава (100 → Dist25=100)
    // Исполнитель помех: L"cpu" (default, без DLL) | L"frei0r" (цепочка
    // frei0r-плагинов, нет DLL → fail-open на CPU). Только эти два токена
    // проходят, остальное (включая отсутствие) → cpu.
    std::wstring backend = L"cpu";

    bool operator==(const EffectsSection& o) const
    {
        return enabled == o.enabled && mirror == o.mirror &&
                grayscale == o.grayscale &&
               noise == o.noise && scanlines == o.scanlines &&
               rgbSplit == o.rgbSplit && tracking == o.tracking &&
               vhs == o.vhs && gateweave == o.gateweave && glow == o.glow &&
               denoise == o.denoise && noiseLevel == o.noiseLevel &&
               scanlinesLevel == o.scanlinesLevel &&
               rgbSplitLevel == o.rgbSplitLevel &&
               trackingLevel == o.trackingLevel &&
               gateweaveLevel == o.gateweaveLevel && glowLevel == o.glowLevel &&
               denoiseLevel == o.denoiseLevel && backend == o.backend;
    }
    bool operator!=(const EffectsSection& o) const { return !(*this == o); }
};

struct Settings {
    std::wstring sourceType = L"static"; // L"static" | L"video" | L"camera"
    StaticSection st;
    VideoSection video;
    CameraSection cam;
    bool autostart = true;
    // Качество v2 (фаза vcam-quality-v2/sub1): L"source" (натив источника,
    // default) | L"fixed720p" (v2 = 720p, лесенка только вниз).
    // Cap натива — 4K константа (vcam::VCamNativeCapW/H), UI нет.
    std::wstring quality = L"source";
    // Эффекты хоста (секция effects): смена только эффектов — без переоткрытия
    // источника (отдельная ветка в WorkerProc, см. BeginSwitch по target/quality).
    EffectsSection fx;
    // Глобальный хоткей (секция hotkey): смена — только перерегистрация
    // RegisterHotKey, без переоткрытия источника.
    HotkeySection hotkey;
    // Хоткей записи (секция recordHotkey): смена — только перерегистрация.
    RecordHotkeySection recordHotkey;
    // Путь записи по умолчанию (секция record): смена — без переоткрытия
    // (в SourceConfig не входит; хост читает при старте записи).
    RecordSection record;

    bool operator==(const Settings& o) const
    {
        return sourceType == o.sourceType && st == o.st && video == o.video &&
                cam == o.cam && autostart == o.autostart && quality == o.quality &&
                fx == o.fx && hotkey == o.hotkey &&
                recordHotkey == o.recordHotkey && record == o.record;
    }
    bool operator!=(const Settings& o) const { return !(*this == o); }

    // Читает новую схему; без секций source/static/video/camera мигрирует из
    // старого формата (imagePath/mediaMode/mediaPath/scaleMode/crop*). false = файла нет/пуст.
    bool Load(const std::wstring& path);
    // Пишет ТОЛЬКО новую схему, UTF-8 без BOM.
    bool Save(const std::wstring& path) const;
    // Сериализация в новую схему (то же, что пишет Save) — для сравнения в watcher.
    std::string Serialize() const;
};

// %APPDATA%\VCam\settings.json (пусто, если APPDATA недоступен).
std::wstring DefaultSettingsPath();

// Конфиг источника указанного типа из настроек (для L"video" crop-параметры не имеют смысла).
SourceConfig ToSourceConfig(const Settings& s, const std::wstring& type);
SourceConfig ToSourceConfig(const Settings& s); // по s.sourceType
