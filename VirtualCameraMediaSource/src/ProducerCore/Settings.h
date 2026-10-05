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

struct Settings {
    std::wstring sourceType = L"static"; // L"static" | L"video" | L"camera"
    StaticSection st;
    VideoSection video;
    CameraSection cam;
    bool autostart = true;
    // Качество v2 (фаза vcam-quality-v2/sub1): L"source" (натив источника,
    // default) | L"fixed1080p" (v2 = 1080p, лесенка только вниз) |
    // L"fixed720p" (v2 = 720p, лесенка только вниз). Одно на все источники.
    // Cap натива — 4K константа (vcam::VCamNativeCapW/H), UI нет.
    std::wstring quality = L"source";
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
               hotkey == o.hotkey &&
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
