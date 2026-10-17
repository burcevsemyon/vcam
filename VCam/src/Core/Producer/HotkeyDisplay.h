#pragma once

#include <string>

#include <windows.h>

namespace vcam {

// Formats a RegisterHotKey combination (modifier bits + virtual-key) the same
// way the host logs it and the UI shows it: Ctrl/Alt/Shift/Win order, then the
// key name ('A', '1', F1..F24, Space/Enter/Tab/Esc/arrows, else VK 0xNN).
inline std::wstring FormatHotkey(int modifiers, int vk)
{
    std::wstring s;
    if (modifiers & MOD_CONTROL) s += L"Ctrl+";
    if (modifiers & MOD_ALT) s += L"Alt+";
    if (modifiers & MOD_SHIFT) s += L"Shift+";
    if (modifiers & MOD_WIN) s += L"Win+";
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        s += (wchar_t)vk;
    } else if (vk >= VK_F1 && vk <= VK_F24) {
        s += L"F" + std::to_wstring(vk - VK_F1 + 1);
    } else {
        switch (vk) {
        case VK_SPACE: s += L"Space"; break;
        case VK_RETURN: s += L"Enter"; break;
        case VK_TAB: s += L"Tab"; break;
        case VK_ESCAPE: s += L"Esc"; break;
        case VK_LEFT: s += L"Left"; break;
        case VK_RIGHT: s += L"Right"; break;
        case VK_UP: s += L"Up"; break;
        case VK_DOWN: s += L"Down"; break;
        default: {
            static const wchar_t* hex = L"0123456789ABCDEF";
            unsigned v = (unsigned)(vk & 0xFF);
            s += L"VK 0x";
            s += hex[(v >> 4) & 0xF];
            s += hex[v & 0xF];
            break;
        }
        }
    }
    return s;
}

} // namespace vcam
