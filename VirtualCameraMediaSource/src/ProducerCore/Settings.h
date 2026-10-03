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
    // Степень помех 0–100 (индивидуальные уровни; VHS использует их же,
    // отдельного vhsLevel нет). Отсутствует в JSON → 100, кламп при чтении.
    // Уровень 0 при включённом тоггле ≈ эффект выключен.
    int noiseLevel = 100;      // амплитуда шума ±(level% от ±60)
    int scanlinesLevel = 100;  // глубина затемнения нечётных строк
    int rgbSplitLevel = 100;   // dx сдвига (100 → 6/12/24 по ширине)
    int trackingLevel = 100;   // число/ширина полос (0 → нет полос)
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
               vhs == o.vhs && noiseLevel == o.noiseLevel &&
               scanlinesLevel == o.scanlinesLevel &&
               rgbSplitLevel == o.rgbSplitLevel &&
               trackingLevel == o.trackingLevel && backend == o.backend;
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

    bool operator==(const Settings& o) const
    {
        return sourceType == o.sourceType && st == o.st && video == o.video &&
                cam == o.cam && autostart == o.autostart && quality == o.quality &&
                fx == o.fx;
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
