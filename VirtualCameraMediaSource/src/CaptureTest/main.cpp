#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

extern int RunFlickerMode(const ModeArgs& a);
extern int RunCompareMode(const ModeArgs& a);
extern int RunAnalyzeMode(const ModeArgs& a);
extern int RunPerfMode(const ModeArgs& a);
extern int RunFormatsMode(const ModeArgs& a);
extern int RunStressMode(const ModeArgs& a);
extern int RunMulticonsumerMode(const ModeArgs& a);
extern int RunHotswitchMode(const ModeArgs& a);
extern int RunScenarioMode(const wchar_t* scriptPath);
extern int RunDirectMode(int numFrames, const wchar_t* outputPrefix);
extern int RunDeviceMode(int numFrames, const wchar_t* nameFilter, UINT32 reqW, UINT32 reqH,
                         const wchar_t* outputPrefix, bool strict, bool wantNv12);
extern int RunInspectMode();

static bool IsAllDigits(const wchar_t* s)
{
    if (!s || *s == L'\0') return false;
    for (const wchar_t* c = s; *c; ++c) if (*c < L'0' || *c > L'9') return false;
    return true;
}

int wmain(int argc, wchar_t* argv[])
{
    wprintf(L"VCam Capture Test - captures frames from the virtual camera\n");
    wprintf(L"Usage: CaptureTest.exe <mode> [options]\n");
    wprintf(L"       CaptureTest.exe [numFrames] [outputPrefix]   (direct, legacy)\n");
    wprintf(L"       CaptureTest.exe device [strict] [nv12] [nameFilter|index] [width] [height] [outputPrefix]\n");
    wprintf(L"       CaptureTest.exe inspect\n");
    wprintf(L"Modes: flicker compare analyze perf formats stress multiconsumer hotswitch scenario\n");
    wprintf(L"\n");

    if (argc < 2) {
        wprintf(L"Usage: CaptureTest.exe <mode> [options]\n");
        wprintf(L"Modes: flicker compare analyze perf formats stress multiconsumer hotswitch scenario\n");
        return 2;
    }

    const wchar_t* mode = argv[1];

    // --- legacy-режимы для e2e_test.ps1 (см. legacy.cpp) ---
    if (_wcsicmp(mode, L"inspect") == 0) {
        int rc = RunInspectMode();
        wprintf(L"inspect exit code: %d\n", rc);
        return rc;
    }
    if (_wcsicmp(mode, L"device") == 0) {
        // strict => точное совпадение имени, без unnamed-fallback
        // nv12   => договариваться о NV12 вместо RGB32 и сохранять через YUV->RGB
        bool strict = false;
        bool nv12 = false;
        int a = 2;
        while (argc > a) {
            if (_wcsicmp(argv[a], L"strict") == 0) { strict = true; ++a; }
            else if (_wcsicmp(argv[a], L"nv12") == 0) { nv12 = true; ++a; }
            else break;
        }
        const wchar_t* nameFilter = (argc > a) ? argv[a] : L"VCam";
        UINT32 reqW = (argc > a + 1) ? (UINT32)_wtoi(argv[a + 1]) : 0;
        UINT32 reqH = (argc > a + 2) ? (UINT32)_wtoi(argv[a + 2]) : 0;
        const wchar_t* outputPrefix = (argc > a + 3) ? argv[a + 3] : L"frame";
        int rc = RunDeviceMode(10, nameFilter, reqW, reqH, outputPrefix, strict, nv12);
        wprintf(L"device mode exit code: %d\n", rc);
        return rc;
    }
    if (IsAllDigits(mode)) {
        // direct-режим (legacy): CaptureTest.exe <numFrames> [outputPrefix]
        int numFrames = _wtoi(mode);
        const wchar_t* outputPrefix = (argc >= 3) ? argv[2] : L"frame";
        return RunDirectMode(numFrames, outputPrefix);
    }

    if (_wcsicmp(mode, L"flicker") == 0) {
        ModeArgs a = ParseModeArgs(argc, argv, 2, 300, 100);
        return RunFlickerMode(a);
    }
    if (_wcsicmp(mode, L"compare") == 0) {
        ModeArgs a = ParseModeArgs(argc, argv, 2, 300, 100);
        return RunCompareMode(a);
    }
    if (_wcsicmp(mode, L"analyze") == 0) {
        ModeArgs a = ParseModeArgs(argc, argv, 2, 100, 100);
        return RunAnalyzeMode(a);
    }
    if (_wcsicmp(mode, L"perf") == 0) {
        ModeArgs a = ParseModeArgs(argc, argv, 2, 300, 0);
        return RunPerfMode(a);
    }
    if (_wcsicmp(mode, L"formats") == 0) {
        ModeArgs a;
        a.json = (argc > 2 && _wcsicmp(argv[2], L"--json") == 0);
        return RunFormatsMode(a);
    }
    if (_wcsicmp(mode, L"stress") == 0) {
        ModeArgs a = ParseModeArgs(argc, argv, 2, 300, 100);
        return RunStressMode(a);
    }
    if (_wcsicmp(mode, L"multiconsumer") == 0) {
        ModeArgs a;
        a.nameFilter = (argc > 2) ? argv[2] : L"VCam";
        return RunMulticonsumerMode(a);
    }
    if (_wcsicmp(mode, L"hotswitch") == 0) {
        ModeArgs a = ParseModeArgs(argc, argv, 2, 60, 100);
        return RunHotswitchMode(a);
    }
    if (_wcsicmp(mode, L"scenario") == 0) {
        if (argc < 3) {
            wprintf(L"Usage: CaptureTest.exe scenario <script.txt>\n");
            return 2;
        }
        return RunScenarioMode(argv[2]);
    }

    wprintf(L"Unknown mode: %s\n", mode);
    return 2;
}