#include "Mp4Recorder.h"
#include "ImageLayout.h"

#include <ctime>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")

namespace {

bool EnsureParentDir(const std::wstring& path)
{
    size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash < 3) return true; // "C:\x" — корень жив
    std::wstring dir = path.substr(0, slash);
    // CreateDirectory по цепочке: идём вверх до существующего, потом вниз.
    std::vector<std::wstring> chain;
    std::wstring cur = dir;
    for (int i = 0; i < 32; i++) {
        DWORD a = GetFileAttributesW(cur.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) break;
        chain.push_back(cur);
        size_t s = cur.find_last_of(L"\\/");
        if (s == std::wstring::npos || s < 3) break;
        cur = cur.substr(0, s);
    }
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        if (!CreateDirectoryW(it->c_str(), nullptr) &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            return false;
    }
    DWORD a = GetFileAttributesW(dir.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

} // namespace

Mp4Recorder::Mp4Recorder() = default;

Mp4Recorder::~Mp4Recorder()
{
    Stop();
}

std::wstring Mp4Recorder::HrHex(HRESULT hr)
{
    wchar_t b[16];
    swprintf_s(b, L"0x%08X", (unsigned)hr);
    return b;
}

bool Mp4Recorder::Start(const std::wstring& path, std::wstring& err)
{
    Stop(); // переоткрытие поверх — сначала финализировать старое
    if (path.empty()) {
        err = L"empty record path";
        return false;
    }
    if (!EnsureParentDir(path)) {
        err = L"cannot create directory for: " + path;
        return false;
    }
    DeleteFileW(path.c_str()); // SinkWriter перезаписывает, но чище начать с нуля

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hrCo) && hrCo != RPC_E_CHANGED_MODE) {
        err = L"CoInitializeEx failed: " + HrHex(hrCo);
        return false;
    }
    comUp_ = SUCCEEDED(hrCo);

    HRESULT hrMf = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hrMf)) {
        err = L"MFStartup failed: " + HrHex(hrMf);
        if (comUp_) { CoUninitialize(); comUp_ = false; }
        return false;
    }
    mfUp_ = true;

    ATL::CComPtr<IMFSinkWriter> writer;
    HRESULT hr = MFCreateSinkWriterFromURL(path.c_str(), nullptr, nullptr, &writer);
    if (FAILED(hr) || !writer) {
        err = L"MFCreateSinkWriterFromURL failed: " + HrHex(hr);
        Stop();
        return false;
    }

    // Выход: H.264 1280x720@30, progressive, 8 Мбит/с (.mp4 по расширению).
    ATL::CComPtr<IMFMediaType> outType;
    hr = MFCreateMediaType(&outType);
    if (SUCCEEDED(hr)) hr = outType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = outType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    if (SUCCEEDED(hr)) hr = outType->SetUINT32(MF_MT_AVG_BITRATE, (UINT32)kBitrate);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(outType, MF_MT_FRAME_SIZE, kW, kH);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(outType, MF_MT_FRAME_RATE, kFps, 1);
    if (SUCCEEDED(hr))
        hr = outType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr))
        hr = MFSetAttributeRatio(outType, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    DWORD stream = 0;
    if (SUCCEEDED(hr)) hr = writer->AddStream(outType, &stream);
    if (FAILED(hr) || !outType) {
        err = L"record AddStream(H264) failed: " + HrHex(hr);
        Stop();
        return false;
    }

    // Вход: packed RGB32 1280x720. Конвертер SinkWriter→H.264 ждёт bottom-up
    // DIB при входном типе без MF_MT_DEFAULT_STRIDE; игры со страйдом не
    // помогают (отрицательный страйд, 2026-10-03: выход побитово тот же —
    // rec_orient_after.mp4 остался перевёрнутым, верх синий R=2 B=243).
    // Поэтому top-down кэш переворачиваем построчно при копии в WriteSample.
    // Эмпирика 2026-10-03: синтетика top-down верх-красный/низ-синий →
    // Mp4Recorder → финализация → ffmpeg rawvideo rgb24 (top-down по
    // определению): верх СИНИЙ R=2 B=243 (rec_orient_before.mp4) — переворот
    // доказан; после построчного флипа верх КРАСНЫЙ (rec_orient_fixed.mp4).
    // Судья — %TEMP%\opencode\vcam-orient\orient_check.ps1 (ffmpeg + MF
    // SourceReader с явным учётом знака выходного страйда +5120).
    ATL::CComPtr<IMFMediaType> inType;
    hr = MFCreateMediaType(&inType);
    if (SUCCEEDED(hr)) hr = inType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    if (SUCCEEDED(hr)) hr = inType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    if (SUCCEEDED(hr)) hr = MFSetAttributeSize(inType, MF_MT_FRAME_SIZE, kW, kH);
    if (SUCCEEDED(hr)) hr = MFSetAttributeRatio(inType, MF_MT_FRAME_RATE, kFps, 1);
    if (SUCCEEDED(hr))
        hr = inType->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    if (SUCCEEDED(hr)) hr = writer->SetInputMediaType(stream, inType, nullptr);
    if (FAILED(hr)) {
        err = L"record SetInputMediaType(RGB32) failed: " + HrHex(hr);
        Stop();
        return false;
    }

    hr = writer->BeginWriting();
    if (FAILED(hr)) {
        err = L"record BeginWriting failed: " + HrHex(hr);
        Stop();
        return false;
    }

    if (!QueryPerformanceFrequency((LARGE_INTEGER*)&qpcFreq_) || qpcFreq_ <= 0)
        qpcFreq_ = 10000000;
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    startQpc_ = qpc.QuadPart;
    lastTs_ = -1;

    writer_ = writer;
    stream_ = stream;
    path_ = path;
    startTickMs_ = GetTickCount64();
    frames_ = 0;
    dropped_ = 0;
    debtMs_ = 0.0;
    lastErr_.clear();
    open_ = true;
    err.clear();
    return true;
}

bool Mp4Recorder::WriteFrame720p(const uint8_t* bgrxTopDown)
{
    if (!open_ || !writer_ || !bgrxTopDown) return false;

    // Эфир важнее: входящий кадр "гасит" 33.33 мс долга; долг сверх порога —
    // дроп (не пишем, счётчик растёт, метки следующих честно прыгают дальше).
    debtMs_ -= 1000.0 / kFps;
    if (debtMs_ < -1000.0) debtMs_ = -1000.0; // без бесконечного кредита
    if (debtMs_ > kDropDebtMs) {
        dropped_++;
        return true;
    }

    LARGE_INTEGER t0 = {};
    QueryPerformanceCounter(&t0);
    bool ok = WriteSample(bgrxTopDown);
    LARGE_INTEGER t1 = {};
    QueryPerformanceCounter(&t1);
    if (ok)
        debtMs_ += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qpcFreq_;
    return ok;
}

bool Mp4Recorder::WriteFrameNative(const uint8_t* bgrx, int stride, uint32_t w, uint32_t h)
{
    if (!open_ || !writer_ || !bgrx) return false;
    if (w == kW && h == kH && stride == (int)kStride) {
        return WriteFrame720p(bgrx);
    }
    static thread_local std::vector<uint8_t> scratch;
    if (scratch.size() < kFrameSize) {
        try {
            scratch.resize(kFrameSize);
        } catch (...) {
            return false;
        }
    }
    vcam::LetterboxBilinearEx(bgrx, w, h, (LONG)stride, scratch.data(), kW, kH, (LONG)kStride);
    return WriteFrame720p(scratch.data());
}

bool Mp4Recorder::WriteSample(const uint8_t* bgrxTopDown)
{
    LARGE_INTEGER qpc = {};
    QueryPerformanceCounter(&qpc);
    LONGLONG ts = (qpc.QuadPart - startQpc_) * 10000000LL / qpcFreq_;
    if (ts <= lastTs_) ts = lastTs_ + 1; // строго монотонно
    lastTs_ = ts;

    ATL::CComPtr<IMFMediaBuffer> buf;
    HRESULT hr = MFCreateMemoryBuffer((DWORD)kFrameSize, &buf);
    if (FAILED(hr) || !buf) {
        lastErr_ = L"MFCreateMemoryBuffer failed: " + HrHex(hr);
        return false;
    }
    BYTE* dst = nullptr;
    DWORD maxLen = 0, curLen = 0;
    hr = buf->Lock(&dst, &maxLen, &curLen);
    if (FAILED(hr) || !dst || maxLen < kFrameSize) {
        if (dst) buf->Unlock();
        lastErr_ = L"record buffer lock failed: " + HrHex(hr);
        return false;
    }
    // Конвертер ждёт bottom-up (см. комментарий в Start): top-down кэш
    // переворачиваем построчно (байт в байт тот же объём, +~0.3 мс/кадр).
    // Прямой memcpy всего кадра давал видео вверх ногами
    // (доказано rec_orient_before.mp4, 2026-10-03).
    for (size_t y = 0; y < kH; y++) {
        memcpy(dst + y * kStride,
               bgrxTopDown + (size_t)(kH - 1 - (uint32_t)y) * kStride,
               kStride);
    }
    buf->Unlock();
    hr = buf->SetCurrentLength((DWORD)kFrameSize);
    if (FAILED(hr)) {
        lastErr_ = L"record SetCurrentLength failed: " + HrHex(hr);
        return false;
    }

    ATL::CComPtr<IMFSample> sample;
    hr = MFCreateSample(&sample);
    if (SUCCEEDED(hr)) hr = sample->AddBuffer(buf);
    if (SUCCEEDED(hr)) hr = sample->SetSampleTime(ts);
    if (SUCCEEDED(hr)) hr = sample->SetSampleDuration((LONGLONG)kFrameDur100ns);
    if (SUCCEEDED(hr)) hr = writer_->WriteSample(stream_, sample);
    if (FAILED(hr)) {
        lastErr_ = L"record WriteSample failed: " + HrHex(hr);
        return false;
    }
    frames_++;
    return true;
}

void Mp4Recorder::Stop()
{
    if (open_ && writer_) {
        HRESULT hr = writer_->Finalize(); // без этого mp4 битый
        if (FAILED(hr) && lastErr_.empty())
            lastErr_ = L"record Finalize failed: " + HrHex(hr);
    }
    writer_.Release();
    open_ = false;
    if (mfUp_) {
        MFShutdown(); // парный MFStartup из Start
        mfUp_ = false;
    }
    if (comUp_) {
        CoUninitialize(); // парный CoInitializeEx из Start
        comUp_ = false;
    }
}
