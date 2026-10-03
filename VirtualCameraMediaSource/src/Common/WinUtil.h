#pragma once

#include <windows.h>

#include <string>

// Единые формат-хелперы (раньше дублировались в CameraControls/CameraSource/
// VideoFileSource/VideoProcessorScaler/VideoProducer/ControlServer/
// ControlProxyClient): один источник, везде swprintf_s.
namespace vcam {

inline std::wstring HrHex(HRESULT hr)
{
    wchar_t buf[16];
    swprintf_s(buf, L"0x%08X", static_cast<unsigned>(hr));
    return std::wstring(buf);
}

inline std::wstring WinErr(DWORD e)
{
    wchar_t buf[64];
    swprintf_s(buf, L"win32 %lu", static_cast<unsigned long>(e));
    return std::wstring(buf);
}

} // namespace vcam
