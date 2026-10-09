#include "doctest.h"
#include <windows.h>
#include "TraySourceMenu.h"

#include <cstdio>
#include <string>

// Чтение файла целиком для сравнения «байт в байт».
static std::string ReadAllBytes(const std::wstring& path)
{
    std::string out;
    FILE* f = _wfopen(path.c_str(), L"rb");
    if (!f) return out;
    char buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        out.append(buf, n);
    fclose(f);
    return out;
}

TEST_CASE("tray menu: source submenu items and checkmark")
{
    struct { const wchar_t* type; UINT checkedId; } cases[] = {
        { L"static", 107 }, { L"video", 108 }, { L"camera", 109 },
    };
    for (auto& c : cases)
    {
        HMENU parent = CreatePopupMenu();
        REQUIRE(parent != nullptr);

        Settings s;
        s.sourceType = c.type;
        AppendSourceSubmenu(parent, s, 107, 108, 109);

        // подменю вставлено единственным пунктом
        CHECK(GetMenuItemCount(parent) == 1);

        MENUITEMINFOW sub = {};
        sub.cbSize = sizeof(sub);
        sub.fMask = MIIM_SUBMENU;
        REQUIRE(GetMenuItemInfoW(parent, 0, TRUE, &sub));
        REQUIRE(sub.hSubMenu != nullptr);

        wchar_t header[64] = {};
        GetMenuStringW(parent, 0, header, 64, MF_BYPOSITION);
        CHECK(std::wstring(header) == L"Источник");

        REQUIRE(GetMenuItemCount(sub.hSubMenu) == 3);
        struct { UINT id; const wchar_t* text; } want[] = {
            { 107, L"Static\tCtrl+Alt+1" }, { 108, L"Video\tCtrl+Alt+2" },
            { 109, L"Camera\tCtrl+Alt+3" },
        };
        for (UINT i = 0; i < 3; ++i)
        {
            MENUITEMINFOW it = {};
            it.cbSize = sizeof(it);
            it.fMask = MIIM_ID | MIIM_STATE;
            REQUIRE(GetMenuItemInfoW(sub.hSubMenu, i, TRUE, &it));
            CHECK(it.wID == want[i].id);

            wchar_t buf[64] = {};
            GetMenuStringW(sub.hSubMenu, it.wID, buf, 64, MF_BYCOMMAND);
            CHECK(std::wstring(buf) == want[i].text);

            bool checked = (it.fState & MF_CHECKED) != 0;
            CHECK(checked == (it.wID == c.checkedId));
        }

        DestroyMenu(parent);
    }
}

TEST_CASE("tray menu: source submenu shows current hotkey combinations")
{
    HMENU parent = CreatePopupMenu();
    REQUIRE(parent != nullptr);

    Settings s;
    s.sourceStaticHotkey = { 6, 0x41 }; // Ctrl+Shift+A
    s.sourceVideoHotkey = { 1, 0x70 };  // Alt+F1
    s.sourceCameraHotkey = { 3, 0x20 }; // Ctrl+Alt+Space
    AppendSourceSubmenu(parent, s, 107, 108, 109);

    MENUITEMINFOW sub = {};
    sub.cbSize = sizeof(sub);
    sub.fMask = MIIM_SUBMENU;
    REQUIRE(GetMenuItemInfoW(parent, 0, TRUE, &sub));
    REQUIRE(sub.hSubMenu != nullptr);

    wchar_t buf[64] = {};
    GetMenuStringW(sub.hSubMenu, 107, buf, 64, MF_BYCOMMAND);
    CHECK(std::wstring(buf) == L"Static\tCtrl+Shift+A");
    GetMenuStringW(sub.hSubMenu, 108, buf, 64, MF_BYCOMMAND);
    CHECK(std::wstring(buf) == L"Video\tAlt+F1");
    GetMenuStringW(sub.hSubMenu, 109, buf, 64, MF_BYCOMMAND);
    CHECK(std::wstring(buf) == L"Camera\tCtrl+Alt+Space");

    DestroyMenu(parent);
}

TEST_CASE("tray menu: ApplySourceSwitch saves once, then unchanged")
{
    wchar_t tmp[MAX_PATH];
    REQUIRE(GetTempPathW(MAX_PATH, tmp) != 0);
    wchar_t file[MAX_PATH];
    REQUIRE(GetTempFileNameW(tmp, L"vct", 0, file) != 0);
    std::wstring path = file;

    Settings s;
    s.sourceType = L"static";
    REQUIRE(s.Save(path));

    // первое переключение — запись
    CHECK(ApplySourceSwitch(path, L"video") == SourceSwitchResult::Saved);
    Settings after;
    REQUIRE(after.Load(path));
    CHECK(after.sourceType == L"video");

    // повторный выбор того же типа — без Save, файл байт-в-байт тот же
    std::string before = ReadAllBytes(path);
    CHECK(ApplySourceSwitch(path, L"video") == SourceSwitchResult::Unchanged);
    CHECK(ReadAllBytes(path) == before);

    // несуществующий файл — LoadFailed
    std::wstring missing = std::wstring(tmp) + L"vcam_tray_missing.json";
    DeleteFileW(missing.c_str());
    CHECK(ApplySourceSwitch(missing, L"video") == SourceSwitchResult::LoadFailed);

    DeleteFileW(path.c_str());
}
