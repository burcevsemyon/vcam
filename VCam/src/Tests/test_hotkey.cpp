#include "doctest.h"
#include "Settings.h"

#include <windows.h>
#include <cstdio>
#include <string>

// Hotkey/record parse: мусор -> дефолт, границы vk, модификаторы 1-15.
// Правила зеркалят C# Settings.ParseHotkeyModifiers/ParseHotkeyVk.

static Settings LoadJson(const char* json)
{
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring path = std::wstring(tmp) + L"vcam_test_hk.json";
    FILE* f = _wfopen(path.c_str(), L"wb");
    REQUIRE(f != nullptr);
    fwrite(json, 1, strlen(json), f);
    fclose(f);
    Settings s;
    s.Load(path);
    DeleteFileW(path.c_str());
    return s;
}

TEST_CASE("hotkey: default Ctrl+Alt+V when missing")
{
    Settings s = LoadJson("{}");
    CHECK(s.hotkey.modifiers == 3);  // MOD_CONTROL | MOD_ALT
    CHECK(s.hotkey.vk == 0x56);      // 'V'
}

TEST_CASE("hotkey: valid values pass through")
{
    Settings s = LoadJson(R"({"static":{},"hotkey":{"modifiers":6,"vk":65}})");
    CHECK(s.hotkey.modifiers == 6);
    CHECK(s.hotkey.vk == 65);
}

TEST_CASE("hotkey: modifiers clamped to 1-15")
{
    // 0 -> default, 16 -> default, 15 -> pass
    Settings s0 = LoadJson(R"({"static":{},"hotkey":{"modifiers":0,"vk":65}})");
    CHECK(s0.hotkey.modifiers == 3);
    Settings s16 = LoadJson(R"({"static":{},"hotkey":{"modifiers":16,"vk":65}})");
    CHECK(s16.hotkey.modifiers == 3);
    Settings s15 = LoadJson(R"({"static":{},"hotkey":{"modifiers":15,"vk":65}})");
    CHECK(s15.hotkey.modifiers == 15);
    Settings s1 = LoadJson(R"({"static":{},"hotkey":{"modifiers":1,"vk":65}})");
    CHECK(s1.hotkey.modifiers == 1);
}

TEST_CASE("hotkey: vk clamped to 0x08-0xFE")
{
    // 0 -> default, 7 -> default, 8 -> pass, 0xFE -> pass, 0xFF -> default
    Settings s0 = LoadJson(R"({"static":{},"hotkey":{"modifiers":3,"vk":0}})");
    CHECK(s0.hotkey.vk == 0x56);
    Settings s7 = LoadJson(R"({"static":{},"hotkey":{"modifiers":3,"vk":7}})");
    CHECK(s7.hotkey.vk == 0x56);
    Settings s8 = LoadJson(R"({"static":{},"hotkey":{"modifiers":3,"vk":8}})");
    CHECK(s8.hotkey.vk == 8);
    Settings sFE = LoadJson(R"({"static":{},"hotkey":{"modifiers":3,"vk":254}})");
    CHECK(sFE.hotkey.vk == 254);
    Settings sFF = LoadJson(R"({"static":{},"hotkey":{"modifiers":3,"vk":255}})");
    CHECK(sFF.hotkey.vk == 0x56);
}

TEST_CASE("recordHotkey: default Ctrl+Alt+R when missing")
{
    Settings s = LoadJson("{}");
    CHECK(s.recordHotkey.modifiers == 3);
    CHECK(s.recordHotkey.vk == 0x52); // 'R'
}

TEST_CASE("recordHotkey: valid values pass through")
{
    Settings s = LoadJson(R"({"static":{},"recordHotkey":{"modifiers":7,"vk":82}})");
    CHECK(s.recordHotkey.modifiers == 7);
    CHECK(s.recordHotkey.vk == 82);
}

TEST_CASE("recordHotkey: vk clamped to 0x08-0xFE (default 0x52)")
{
    Settings s0 = LoadJson(R"({"static":{},"recordHotkey":{"modifiers":3,"vk":0}})");
    CHECK(s0.recordHotkey.vk == 0x52);
    Settings sFF = LoadJson(R"({"static":{},"recordHotkey":{"modifiers":3,"vk":255}})");
    CHECK(sFF.recordHotkey.vk == 0x52);
    Settings s8 = LoadJson(R"({"static":{},"recordHotkey":{"modifiers":3,"vk":8}})");
    CHECK(s8.recordHotkey.vk == 8);
}

TEST_CASE("videoHotkey: default Ctrl+Alt+P when missing")
{
    Settings s = LoadJson("{}");
    CHECK(s.videoHotkey.modifiers == 3);
    CHECK(s.videoHotkey.vk == 0x50); // 'P'
}

TEST_CASE("videoHotkey: valid values pass through")
{
    Settings s = LoadJson(R"({"static":{},"videoHotkey":{"modifiers":7,"vk":80}})");
    CHECK(s.videoHotkey.modifiers == 7);
    CHECK(s.videoHotkey.vk == 80);
}

TEST_CASE("videoHotkey: vk clamped to 0x08-0xFE (default 0x50)")
{
    Settings s0 = LoadJson(R"({"static":{},"videoHotkey":{"modifiers":3,"vk":0}})");
    CHECK(s0.videoHotkey.vk == 0x50);
    Settings sFF = LoadJson(R"({"static":{},"videoHotkey":{"modifiers":3,"vk":255}})");
    CHECK(sFF.videoHotkey.vk == 0x50);
    Settings s8 = LoadJson(R"({"static":{},"videoHotkey":{"modifiers":3,"vk":8}})");
    CHECK(s8.videoHotkey.vk == 8);
    Settings sMods = LoadJson(R"({"static":{},"videoHotkey":{"modifiers":16,"vk":80}})");
    CHECK(sMods.videoHotkey.modifiers == 3);
}

TEST_CASE("source switch hotkeys: defaults Ctrl+Alt+1/2/3 when missing")
{
    Settings s = LoadJson("{}");
    CHECK(s.sourceStaticHotkey.modifiers == 3);
    CHECK(s.sourceStaticHotkey.vk == 0x31); // '1'
    CHECK(s.sourceVideoHotkey.modifiers == 3);
    CHECK(s.sourceVideoHotkey.vk == 0x32); // '2'
    CHECK(s.sourceCameraHotkey.modifiers == 3);
    CHECK(s.sourceCameraHotkey.vk == 0x33); // '3'
}

TEST_CASE("source switch hotkeys: valid values pass through")
{
    Settings s = LoadJson(R"({"static":{},"sourceStaticHotkey":{"modifiers":6,"vk":65},)"
                          R"("sourceVideoHotkey":{"modifiers":7,"vk":66},)"
                          R"("sourceCameraHotkey":{"modifiers":5,"vk":67}})");
    CHECK(s.sourceStaticHotkey.modifiers == 6);
    CHECK(s.sourceStaticHotkey.vk == 65);
    CHECK(s.sourceVideoHotkey.modifiers == 7);
    CHECK(s.sourceVideoHotkey.vk == 66);
    CHECK(s.sourceCameraHotkey.modifiers == 5);
    CHECK(s.sourceCameraHotkey.vk == 67);
}

TEST_CASE("source switch hotkeys: vk/mods clamp to per-section default")
{
    Settings s0 = LoadJson(R"({"static":{},"sourceVideoHotkey":{"modifiers":3,"vk":0}})");
    CHECK(s0.sourceVideoHotkey.vk == 0x32);
    Settings sFF = LoadJson(R"({"static":{},"sourceCameraHotkey":{"modifiers":3,"vk":255}})");
    CHECK(sFF.sourceCameraHotkey.vk == 0x33);
    Settings s8 = LoadJson(R"({"static":{},"sourceStaticHotkey":{"modifiers":3,"vk":8}})");
    CHECK(s8.sourceStaticHotkey.vk == 8);
    Settings sMods = LoadJson(R"({"static":{},"sourceStaticHotkey":{"modifiers":16,"vk":65}})");
    CHECK(sMods.sourceStaticHotkey.modifiers == 3);
}
