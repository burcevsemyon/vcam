#include <windows.h>
#include <guiddef.h>

extern "C" const IID IID_IMFMediaSource = { 0x279a808d, 0xaec7, 0x40c8, 0x9c, 0x6b, 0xa6, 0xb4, 0x92, 0xc7, 0x8a, 0x66 };

static const IID* const s_MfIidsKeepAlive[] = { &IID_IMFMediaSource };
