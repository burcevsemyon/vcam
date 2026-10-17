#include "doctest.h"
#include <windows.h>

#include "Settings.h"

#include <cstdio>
#include <string>

// Edge-cases Settings.Load: пустой файл, битый JSON, огромный файл,
// отсутствующий файл. Ничего не должно падать — всегда дефолт или
// частичный парсинг.

static std::wstring TempPath(const wchar_t* name)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    return std::wstring(tmp) + name;
}

static void WriteFile(const std::wstring& path, const char* content)
{
    FILE* f = _wfopen(path.c_str(), L"wb");
    REQUIRE(f != nullptr);
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

TEST_CASE("settings: empty file returns defaults")
{
    auto path = TempPath(L"vcam_edge_empty.json");
    WriteFile(path, "");
    Settings s;
    s.Load(path.c_str());
    CHECK(s.sourceType == L"static");
    CHECK(s.quality == L"source");
    CHECK(s.st.scaleMode == L"fit");
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: malformed JSON returns defaults")
{
    auto path = TempPath(L"vcam_edge_malformed.json");
    WriteFile(path, "{this is not json at all");
    Settings s;
    s.Load(path.c_str());
    CHECK(s.sourceType == L"static");
    CHECK(s.quality == L"source");
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: missing file returns defaults")
{
    Settings s;
    s.Load(L"C:\\nonexistent\\vcam_test.json");
    CHECK(s.sourceType == L"static");
    CHECK(s.quality == L"source");
}

TEST_CASE("settings: empty JSON object returns defaults")
{
    auto path = TempPath(L"vcam_edge_emptyobj.json");
    WriteFile(path, "{}");
    Settings s;
    s.Load(path.c_str());
    CHECK(s.sourceType == L"static");
    CHECK(s.st.scaleMode == L"fit");
    CHECK(s.hotkey.modifiers == 3);
    CHECK(s.hotkey.vk == 0x56);
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: unknown source.type is preserved verbatim")
{
    auto path = TempPath(L"vcam_edge_unknowntype.json");
    WriteFile(path, R"({"static":{},"source":{"type":"future_type"}})");
    Settings s;
    s.Load(path.c_str());
    // C++ stores verbatim (like C#), host falls back to static.
    CHECK(s.sourceType == L"future_type");
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: null/empty strings don't crash")
{
    auto path = TempPath(L"vcam_edge_nulls.json");
    WriteFile(path, R"({"static":{"path":"","scaleMode":"","cropX":0}})");
    Settings s;
    s.Load(path.c_str());
    CHECK(s.st.path.empty());
    CHECK(s.st.scaleMode == L"fit"); // empty -> default
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: negative crop values pass through (no clamping in parser)")
{
    auto path = TempPath(L"vcam_edge_negcrop.json");
    WriteFile(path, R"({"static":{"cropX":-5,"cropY":-10,"cropW":-1,"cropH":-2}})");
    Settings s;
    s.Load(path.c_str());
    // Parser stores as-is; clamping happens at render time.
    CHECK(s.st.cropX == -5);
    CHECK(s.st.cropY == -10);
    CHECK(s.st.cropW == -1);
    CHECK(s.st.cropH == -2);
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: very large crop values pass through")
{
    auto path = TempPath(L"vcam_edge_bigcrop.json");
    WriteFile(path, R"({"static":{"cropX":999999,"cropY":999999,"cropW":999999,"cropH":999999}})");
    Settings s;
    s.Load(path.c_str());
    CHECK(s.st.cropX == 999999);
    CHECK(s.st.cropW == 999999);
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: round-trip with empty strings")
{
    Settings s;
    s.sourceType = L"static";
    s.st.path = L"";
    s.st.scaleMode = L"fit";
    s.video.path = L"";
    s.cam.id = L"";
    s.cam.name = L"";
    s.record.path = L"";

    std::string json = s.Serialize();
    auto path = TempPath(L"vcam_edge_rt_empty.json");
    s.Save(path.c_str());
    Settings r;
    r.Load(path.c_str());
    CHECK(r.st.path.empty());
    CHECK(r.video.path.empty());
    CHECK(r.cam.id.empty());
    CHECK(r.cam.name.empty());
    CHECK(r.record.path.empty());
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: autostart false is preserved")
{
    auto path = TempPath(L"vcam_edge_autostart.json");
    WriteFile(path, R"({"static":{},"autostart":false})");
    Settings s;
    s.Load(path.c_str());
    CHECK(s.autostart == false);
    DeleteFileW(path.c_str());
}

TEST_CASE("settings: autostart true is default when missing")
{
    auto path = TempPath(L"vcam_edge_noautostart.json");
    WriteFile(path, R"({"static":{}})");
    Settings s;
    s.Load(path.c_str());
    CHECK(s.autostart == true);
    DeleteFileW(path.c_str());
}

// Crop clamping (концептуально — логика из StaticImageSource::Render).
// В рендере рект клампится к границам нативного кадра.
static void ClampCrop(int& rx, int& ry, int& rw, int& rh,
                      int srcW, int srcH)
{
    if (rx < 0) rx = 0;
    if (ry < 0) ry = 0;
    if (rx >= srcW) rx = 0;
    if (ry >= srcH) ry = 0;
    if (rw <= 0 || rw > srcW - rx) rw = srcW - rx;
    if (rh <= 0 || rh > srcH - ry) rh = srcH - ry;
}

TEST_CASE("croclamp: valid rect passes through")
{
    int rx = 10, ry = 20, rw = 100, rh = 50;
    ClampCrop(rx, ry, rw, rh, 1280, 720);
    CHECK(rx == 10);
    CHECK(ry == 20);
    CHECK(rw == 100);
    CHECK(rh == 50);
}

TEST_CASE("croclamp: negative origin clamped to 0")
{
    int rx = -5, ry = -10, rw = 100, rh = 50;
    ClampCrop(rx, ry, rw, rh, 1280, 720);
    CHECK(rx == 0);
    CHECK(ry == 0);
    CHECK(rw == 100);
    CHECK(rh == 50);
}

TEST_CASE("croclamp: origin beyond source resets to 0")
{
    int rx = 2000, ry = 1000, rw = 100, rh = 50;
    ClampCrop(rx, ry, rw, rh, 1280, 720);
    CHECK(rx == 0);
    CHECK(ry == 0);
}

TEST_CASE("croclamp: oversized width/height clamped to remaining space")
{
    int rx = 1200, ry = 700, rw = 200, rh = 100;
    ClampCrop(rx, ry, rw, rh, 1280, 720);
    CHECK(rw == 80); // 1280 - 1200
    CHECK(rh == 20); // 720 - 700
}

TEST_CASE("croclamp: zero/negative size falls back to full source")
{
    int rx = 10, ry = 10, rw = 0, rh = -5;
    ClampCrop(rx, ry, rw, rh, 1280, 720);
    CHECK(rw == 1270); // 1280 - 10
    CHECK(rh == 710);  // 720 - 10
}

TEST_CASE("croclamp: 1x1 source edge case")
{
    int rx = 0, ry = 0, rw = 1, rh = 1;
    ClampCrop(rx, ry, rw, rh, 1, 1);
    CHECK(rx == 0);
    CHECK(ry == 0);
    CHECK(rw == 1);
    CHECK(rh == 1);
}
