#pragma once

#include <string>

bool PathExists(const std::wstring& p);
std::wstring ExeDirectory();
std::wstring FindHelperExe(const wchar_t* fileName);
void LaunchHelper(const std::wstring& path);
bool LaunchByName(const wchar_t* fileName);
void OpenSettingsUi();
void OpenPreview();
std::wstring ProductVersionString();
void ShowAbout();
