#pragma once

#include <windows.h>

#include "Settings.h"

void OnSettingsChanged(const Settings&);
void ShowTrayMenu(HWND hwnd);
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
