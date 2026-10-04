#pragma once

#include <string>

int RunSchtasks(const std::wstring& args, bool elevate);
std::wstring AutostartTaskArgs(bool enabled);
bool IsAutostartTaskPresent();
bool ApplyAutostart(bool enabled);
void ApplyAutostartFromSettings();
void ToggleAutostart();
