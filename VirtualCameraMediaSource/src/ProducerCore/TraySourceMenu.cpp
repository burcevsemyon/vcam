#include "TraySourceMenu.h"

#include "HotkeyDisplay.h"

namespace {
// Menu label with the hotkey shown as a right-aligned accelerator after a tab.
std::wstring ItemLabel(const wchar_t* label, const SourceSwitchHotkeySection& hk)
{
    return std::wstring(label) + L"\t" + vcam::FormatHotkey(hk.modifiers, hk.vk);
}
} // namespace

void AppendSourceSubmenu(HMENU parent, const Settings& s, UINT idStatic,
                         UINT idVideo, UINT idCamera)
{
    HMENU srcMenu = CreatePopupMenu();
    AppendMenuW(srcMenu, MF_STRING | (s.sourceType == L"static" ? MF_CHECKED : 0),
                idStatic, ItemLabel(L"Static", s.sourceStaticHotkey).c_str());
    AppendMenuW(srcMenu, MF_STRING | (s.sourceType == L"video" ? MF_CHECKED : 0),
                idVideo, ItemLabel(L"Video", s.sourceVideoHotkey).c_str());
    AppendMenuW(srcMenu, MF_STRING | (s.sourceType == L"camera" ? MF_CHECKED : 0),
                idCamera, ItemLabel(L"Camera", s.sourceCameraHotkey).c_str());
    AppendMenuW(parent, MF_POPUP, (UINT_PTR)srcMenu, L"Источник");
}

SourceSwitchResult ApplySourceSwitch(const std::wstring& path, const wchar_t* type)
{
    Settings s;
    if (!s.Load(path))
        return SourceSwitchResult::LoadFailed;
    if (s.sourceType == type)
        return SourceSwitchResult::Unchanged;
    s.sourceType = type;
    if (!s.Save(path))
        return SourceSwitchResult::SaveFailed;
    return SourceSwitchResult::Saved;
}
