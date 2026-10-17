#pragma once

#include <string>

bool PathExists(const std::wstring& p);
std::wstring ExeDirectory();
std::wstring FindHelperExe(const wchar_t* fileName, const wchar_t* rel);
void LaunchHelper(const std::wstring& path);
void OpenSettingsUi();
void OpenPreview();
std::wstring ProductVersionString();
void ShowAbout();
