#pragma once

#define INITGUID
#define NOMINMAX
#define _CRT_SECURE_NO_WARNINGS

#include <windows.h>
#include <cguid.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfobjects.h>
#include <Mferror.h>
#include <atlbase.h>
#include <algorithm>
#include <array>
#include <vector>
#include <string>
#include <psapi.h>
#include <cstdio>
#include <cstdlib>
#include <cstdarg>
#include <cwchar>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "psapi.lib")

#ifndef MF_E_NO_MORE_ITEMS
#define MF_E_NO_MORE_ITEMS MAKE_HRESULT(SEVERITY_ERROR, 0x0811, 0x0001)
#endif