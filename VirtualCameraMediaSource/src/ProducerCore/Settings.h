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

    bool operator==(const CameraSection& o) const { return id == o.id && name == o.name; }
};

struct Settings {
    std::wstring sourceType = L"static"; // L"static" | L"video" | L"camera"
    StaticSection st;
    VideoSection video;
    CameraSection cam;
    bool autostart = true;

    bool operator==(const Settings& o) const
    {
        return sourceType == o.sourceType && st == o.st && video == o.video &&
               cam == o.cam && autostart == o.autostart;
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
