#include "doctest.h"
#include <windows.h>
#include "Settings.h"

#include <cstdio>
#include <string>

// Помощник: сериализация -> парсинг -> сравнение (round-trip).
static Settings RoundTrip(const Settings& s)
{
    std::string json = s.Serialize();
    Settings out;
    // Пишем во временный файл и читаем обратно (Load принимает путь).
    // Используем test-seam: Load(path) уже поддерживает override.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"vcam_test_settings.json";
    s.Save(path);
    out.Load(path);
    DeleteFileW(path.c_str());
    return out;
}

TEST_CASE("settings: default values")
{
    Settings s;
    CHECK(s.sourceType == L"static");
    CHECK(s.st.path.empty());
    CHECK(s.st.scaleMode == L"fit");
    CHECK(s.st.cropX == 0);
    CHECK(s.st.cropY == 0);
    CHECK(s.st.cropW == 0);
    CHECK(s.st.cropH == 0);
    CHECK(s.st.cropKeepAspect == false);
    CHECK(s.quality == L"source");
    CHECK(s.autostart == true);
    CHECK(s.hotkey.modifiers == 3);
    CHECK(s.hotkey.vk == 0x56);
    CHECK(s.recordHotkey.modifiers == 3);
    CHECK(s.recordHotkey.vk == 0x52);
}

TEST_CASE("settings: round-trip preserves all fields")
{
    Settings s;
    s.sourceType = L"video";
    s.st.path = L"C:\\test\\image.png";
    s.st.scaleMode = L"cover";
    s.st.cropX = 10;
    s.st.cropY = 20;
    s.st.cropW = 300;
    s.st.cropH = 200;
    s.st.cropKeepAspect = true;
    s.video.path = L"C:\\test\\video.mp4";
    s.cam.id = L"\\\\?\\usb#vid_1234";
    s.cam.name = L"TestCam";
    s.cam.capture = L"1080p";
    s.quality = L"fixed1080p";
    s.autostart = false;
    s.hotkey.modifiers = 6;
    s.hotkey.vk = 0x41;
    s.recordHotkey.modifiers = 5;
    s.recordHotkey.vk = 0x52;
    s.record.path = L"C:\\test\\out.mp4";

    Settings r = RoundTrip(s);
    CHECK(r.sourceType == L"video");
    CHECK(r.st.path == L"C:\\test\\image.png");
    CHECK(r.st.scaleMode == L"cover");
    CHECK(r.st.cropX == 10);
    CHECK(r.st.cropY == 20);
    CHECK(r.st.cropW == 300);
    CHECK(r.st.cropH == 200);
    CHECK(r.st.cropKeepAspect == true);
    CHECK(r.video.path == L"C:\\test\\video.mp4");
    CHECK(r.cam.id == L"\\\\?\\usb#vid_1234");
    CHECK(r.cam.name == L"TestCam");
    CHECK(r.cam.capture == L"1080p");
    CHECK(r.quality == L"fixed1080p");
    CHECK(r.autostart == false);
    CHECK(r.hotkey.modifiers == 6);
    CHECK(r.hotkey.vk == 0x41);
    CHECK(r.recordHotkey.modifiers == 5);
    CHECK(r.recordHotkey.vk == 0x52);
    CHECK(r.record.path == L"C:\\test\\out.mp4");
}

TEST_CASE("settings: operator== detects changes")
{
    Settings a, b;
    CHECK(a == b);
    b.st.scaleMode = L"cover";
    CHECK(a != b);
    b.st.scaleMode = L"fit";
    CHECK(a == b);
    b.quality = L"fixed720p";
    CHECK(a != b);
    b.quality = L"source";
    CHECK(a == b);
    b.st.cropKeepAspect = true;
    CHECK(a != b);
}

TEST_CASE("settings: parse scaleMode insensitive")
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"vcam_test_sm.json";

    // FIT / Cover / crop — всё должно распознаваться.
    struct { const wchar_t* tok; const wchar_t* want; } cases[] = {
        { L"fit", L"fit" }, { L"FIT", L"fit" }, { L"Fit", L"fit" },
        { L"cover", L"cover" }, { L"COVER", L"cover" }, { L"Cover", L"cover" },
        { L"crop", L"crop" }, { L"CROP", L"crop" },
        { L"garbage", L"fit" }, { L"", L"fit" }, { L"unknown", L"fit" },
    };
    for (auto& c : cases)
    {
        std::string json = std::string("{\"static\":{\"scaleMode\":\"") +
            std::string(c.tok, c.tok + wcslen(c.tok)) + "\"}}";
        FILE* f = _wfopen(path.c_str(), L"wb");
        REQUIRE(f != nullptr);
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
        Settings s;
        s.Load(path);
        CHECK(s.st.scaleMode == c.want);
    }
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: parse quality tokens")
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"vcam_test_q.json";

    struct { const char* tok; const wchar_t* want; } cases[] = {
        { "source", L"source" }, { "fixed720p", L"fixed720p" },
        { "fixed1080p", L"fixed1080p" },
        { "garbage", L"source" }, { "FIXED720P", L"source" }, // ordinal
        { "", L"source" },
    };
    for (auto& c : cases)
    {
        std::string json = std::string("{\"static\":{},\"quality\":\"") + c.tok + "\"}";
        FILE* f = _wfopen(path.c_str(), L"wb");
        REQUIRE(f != nullptr);
        fwrite(json.data(), 1, json.size(), f);
        fclose(f);
        Settings s;
        s.Load(path);
        CHECK(s.quality == c.want);
    }
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: legacy flat format migration")
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"vcam_test_legacy.json";

    std::string json = R"({
        "imagePath": "C:\\img.bmp",
        "mediaMode": "video",
        "mediaPath": "C:\\vid.mp4",
        "scaleMode": "cover",
        "cropX": 5, "cropY": 10, "cropW": 100, "cropH": 50,
        "cropKeepAspect": true,
        "autostart": false
    })";
    FILE* f = _wfopen(path.c_str(), L"wb");
    REQUIRE(f != nullptr);
    fwrite(json.data(), 1, json.size(), f);
    fclose(f);

    Settings s;
    s.Load(path);
    CHECK(s.sourceType == L"video");
    CHECK(s.st.path == L"C:\\img.bmp");
    CHECK(s.video.path == L"C:\\vid.mp4");
    CHECK(s.st.scaleMode == L"cover");
    CHECK(s.st.cropX == 5);
    CHECK(s.st.cropY == 10);
    CHECK(s.st.cropW == 100);
    CHECK(s.st.cropH == 50);
    CHECK(s.st.cropKeepAspect == true);
    CHECK(s.autostart == false);
    CHECK(s.quality == L"source"); // legacy без quality -> source
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: ToSourceConfig static section")
{
    Settings s;
    s.sourceType = L"static";
    s.st.path = L"C:\\img.bmp";
    s.st.scaleMode = L"crop";
    s.st.cropX = 1;
    s.st.cropY = 2;
    s.st.cropW = 3;
    s.st.cropH = 4;
    s.st.cropKeepAspect = true;

    SourceConfig cfg = ToSourceConfig(s);
    CHECK(cfg.type == L"static");
    CHECK(cfg.path == L"C:\\img.bmp");
    CHECK(cfg.scaleMode == L"crop");
    CHECK(cfg.cropX == 1);
    CHECK(cfg.cropY == 2);
    CHECK(cfg.cropW == 3);
    CHECK(cfg.cropH == 4);
    CHECK(cfg.cropKeepAspect == true);
}

TEST_CASE("settings: ToSourceConfig video forces fit")
{
    Settings s;
    s.sourceType = L"video";
    s.video.path = L"C:\\vid.mp4";
    s.st.scaleMode = L"cover"; // static section

    SourceConfig cfg = ToSourceConfig(s);
    CHECK(cfg.type == L"video");
    CHECK(cfg.path == L"C:\\vid.mp4");
    CHECK(cfg.scaleMode == L"fit"); // video always fit
}

TEST_CASE("settings: ToSourceConfig camera normalizes capture")
{
    Settings s;
    s.sourceType = L"camera";
    s.cam.id = L"\\\\?\\usb#1234";
    s.cam.name = L"Cam";
    s.cam.capture = L"garbage";

    SourceConfig cfg = ToSourceConfig(s);
    CHECK(cfg.type == L"camera");
    CHECK(cfg.capture == L"max"); // garbage -> max

    s.cam.capture = L"720p";
    cfg = ToSourceConfig(s);
    CHECK(cfg.capture == L"720p");

    s.cam.capture = L"1080p";
    cfg = ToSourceConfig(s);
    CHECK(cfg.capture == L"1080p");
}
