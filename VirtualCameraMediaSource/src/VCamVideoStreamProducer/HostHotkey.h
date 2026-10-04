#pragma once

#include <string>

template <typename T>
std::wstring HotkeyDisplay(const T& hk);

void WriteHotkeyState(const std::wstring& returnTo);
void ClearHotkeyState();
void ApplyHotkeyRegistration();
bool AutoReturnBorrowedVideo(const std::wstring& settingsPath, const wchar_t* why);
void OnHotkeyPressed();
void OnRecordHotkeyPressed();
