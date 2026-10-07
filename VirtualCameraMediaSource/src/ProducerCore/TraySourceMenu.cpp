#include "TraySourceMenu.h"

void AppendSourceSubmenu(HMENU parent, const Settings& s, UINT idStatic,
                         UINT idVideo, UINT idCamera)
{
    HMENU srcMenu = CreatePopupMenu();
    AppendMenuW(srcMenu, MF_STRING | (s.sourceType == L"static" ? MF_CHECKED : 0),
                idStatic, L"Static");
    AppendMenuW(srcMenu, MF_STRING | (s.sourceType == L"video" ? MF_CHECKED : 0),
                idVideo, L"Video");
    AppendMenuW(srcMenu, MF_STRING | (s.sourceType == L"camera" ? MF_CHECKED : 0),
                idCamera, L"Camera");
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
