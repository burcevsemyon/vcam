#pragma once

#include "CaptureTestTypes.h"

void LogW(const wchar_t* fmt, ...);
void SetLogFile(const wchar_t* path);
PixelStats ComputePixelStats(const BYTE* pBits, DWORD curLen, int w, int h, int stride, double threshold);
void SaveBMP(const wchar_t* filename, const BYTE* rgb32Data, int width, int height, int stride);
ModeArgs ParseModeArgs(int argc, wchar_t** argv, int start, int defFrames, int defInterval);
int OpenDeviceSession(const wchar_t* nameFilter, UINT32 reqW, UINT32 reqH, bool wantNv12,
                      MfSession& session, ATL::CComPtr<IMFActivate>& pAct,
                      ATL::CComPtr<IMFMediaSource>& pSource,
                      ATL::CComPtr<IMFMediaStream>& pStream, Stream0Info& s0);
void CloseDeviceSession(ATL::CComPtr<IMFMediaStream>& pStream,
                        ATL::CComPtr<IMFMediaSource>& pSource,
                        ATL::CComPtr<IMFActivate>& pAct);
int RunCaptureSession(const CapOptions& opt, FrameCallback cb, void* ctx,
                      std::vector<ULONGLONG>* requestMsOut,
                      std::vector<ULONGLONG>* gapsOut);
int FindDevice(const wchar_t* nameFilter, IMFActivate** ppDevices, UINT32 count);
std::wstring JsonEscape(const std::wstring& s);
ULONGLONG FileMtimeMs(const wchar_t* path);
double Percentile(std::vector<ULONGLONG>& v, double p);
bool Sha256Hex(const BYTE* data, DWORD len, wchar_t* out, size_t cch);