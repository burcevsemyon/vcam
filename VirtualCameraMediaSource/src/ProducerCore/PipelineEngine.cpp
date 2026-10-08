#include "PipelineEngine.h"
#include "FailOpenCounters.h"

#include <windows.h>

#include <cstdio>

namespace {

constexpr DWORD kFrameMs = 33;
constexpr DWORD kSwitchWindowMs = 5000;
constexpr DWORD kOpenRetryMs = 250;
constexpr DWORD kFallbackRetryMs = 1000;

} // namespace

PipelineEngine::PipelineEngine() = default;

PipelineEngine::~PipelineEngine()
{
    Close();
}

bool PipelineEngine::Open()
{
    std::wstring err;
    m_writerOpen = m_writer.Open(err);
    if (m_writerOpen) {
        Log(L"shared memory writer ready (" + m_writer.SectionOpenedAs() + L")");
        if (m_writer.IsV2Open())
            Log(L"v2 writer ready (" + m_writer.V2SectionOpenedAs() + L")");
        else
            Log(L"v2 writer unavailable (v1-only mode)");
    } else {
        m_writerErr = err.empty() ? L"writer open failed" : err;
        Log(L"writer open failed: " + err);
    }
    return m_writerOpen;
}

void PipelineEngine::Close()
{
    CloseSource();
    m_writer.Close();
}

void PipelineEngine::SetTarget(const SourceConfig& want, const std::wstring& quality)
{
    const std::wstring normQuality = (quality == L"fixed720p") ? L"fixed720p"
        : (quality == L"fixed1080p")                            ? L"fixed1080p"
                                                                : L"source";
    const std::wstring wantLabel =
        (want.type == L"camera" && !want.camName.empty()) ? want.camName : want.path;
    const std::wstring wasLabel = m_hasTarget
        ? ((m_target.type == L"camera" && !m_target.camName.empty()) ? m_target.camName
                                                                     : m_target.path)
        : std::wstring(L"-");
    // P0.1-наблюдаемость: в switch виден и режим static (scaleMode+crop),
    // иначе по логу нельзя отличить смену ректу от смены пути.
    std::wstring modeLabel;
    if (want.type == L"static") {
        wchar_t rb[128];
        swprintf_s(rb, L" scaleMode=%s crop=(%d,%d,%d,%d)%s",
            want.scaleMode.c_str(), want.cropX, want.cropY, want.cropW, want.cropH,
            want.cropKeepAspect ? L" keepAspect" : L"");
        modeLabel = rb;
    }
    Log(L"switch: type=" + want.type + L" path=" + wantLabel + L" quality=" + normQuality +
        modeLabel +
        L" (was type=" + (m_hasTarget ? m_target.type : std::wstring(L"-")) + L" path=" +
        wasLabel + L" quality=" + (m_hasTarget ? m_quality : std::wstring(L"-")) + L")");
    if (m_hasTarget) m_switchCount++; // P1.2: только реальные переключения, не инит
    CloseSource();
    m_target = want;
    m_quality = normQuality;
    m_writer.SetQuality(m_quality);
    m_nativeKnown = false;
    m_hasTarget = true;
    m_phase = Phase::Switch;
    m_switchStart = GetTickCount64();
    m_nextAttempt = m_switchStart;
}

DWORD PipelineEngine::Step()
{
    ULONGLONG now = GetTickCount64();
    switch (m_phase) {
    case Phase::Switch: {
        if (!m_src) {
            if (now < m_nextAttempt) {
                FlushOrSleep();
                return 0;
            }
            auto cand = CreateSource(m_target.type);
            if (!cand) {
                EnterFallback(L"unknown source type: " + m_target.type);
                return 0;
            }
            std::wstring err;
            if (cand->Open(m_target, err)) {
                m_src = std::move(cand);
                Log(L"source opened: type=" + m_target.type + L" path=" + m_target.path);
            } else {
                m_nextAttempt = now + kOpenRetryMs;
                Log(L"open failed: " + err);
                vcam::IncFailOpen(vcam::FailOpen::SourceOpenFailed);
                if (now - m_switchStart >= kSwitchWindowMs) {
                    EnterFallback(L"open failed: " + err);
                    return 0;
                }
                FlushOrSleep();
                return 0;
            }
        }

        std::wstring rerr;
        if (RenderOne(m_src.get(), rerr)) {
            PostProcessFrame(m_buf.data(), (int)(m_frameW * 4), m_frameW, m_frameH);
            bool written = WriteOne();
            m_phase = Phase::Active;
            Log(L"active: type=" + m_target.type + L" path=" + m_target.path +
                (written ? L"" : L" (write failed)") +
                (m_nativeKnown ? L" [native " : L" [720p ") +
                std::to_wstring(m_frameW) + L"x" + std::to_wstring(m_frameH) +
                L" quality=" + m_quality + L"]");
            return 0;
        }
        if (now - m_switchStart >= kSwitchWindowMs) {
            EnterFallback(L"source produces no frames: " + (rerr.empty() ? L"?" : rerr));
            return 0;
        }
        FlushOrSleep();
        return 0;
    }

    case Phase::Active: {
        std::wstring rerr;
        if (m_src && RenderOne(m_src.get(), rerr)) {
            PostProcessFrame(m_buf.data(), (int)(m_frameW * 4), m_frameW, m_frameH);
            bool written = WriteOne();
            if (!written) {
                Sleep(kFrameMs);
            }
            return 0;
        }
        EnterFallback(rerr.empty() ? L"source produces no frames" : rerr);
        return 0;
    }

    case Phase::Fallback: {
        if (now >= m_nextAttempt) {
            m_nextAttempt = now + kFallbackRetryMs;
            auto cand = CreateSource(m_target.type);
            if (cand) {
                std::wstring err;
                if (cand->Open(m_target, err)) {
                    std::wstring rerr;
                    if (RenderOne(cand.get(), rerr)) {
                        m_src = std::move(cand);
                        PostProcessFrame(m_buf.data(), (int)(m_frameW * 4), m_frameW, m_frameH);
                        bool written = WriteOne();
                        m_phase = Phase::Active;
                        Log(L"signal restored: type=" + m_target.type + L" path=" + m_target.path +
                            (m_nativeKnown ? L" [native " : L" [720p ") +
                            std::to_wstring(m_frameW) + L"x" + std::to_wstring(m_frameH) +
                            L" quality=" + m_quality + L"]");
                        return 0;
                    }
                } else {
                    Log(L"retry open failed: " + err);
                    vcam::IncFailOpen(vcam::FailOpen::SourceOpenFailed);
                }
                cand->Close();
            }
        }
        DWORD wait = 0;
        if (m_nextAttempt > now) wait = (DWORD)(m_nextAttempt - now);
        return wait > 500 ? 500 : wait;
    }
    }
    return 0;
}

void PipelineEngine::CloseSource()
{
    if (m_src) {
        m_src->Close();
        m_src.reset();
    }
}

bool PipelineEngine::ToggleVideoPlay()
{
    if (m_target.type != L"video" || !m_src) return false;
    m_src->PlayPauseToggle();
    if (m_src->Ended())
        Log(L"video: play pressed (restart from beginning)");
    else if (m_src->IsPaused())
        Log(L"video: paused");
    else
        Log(L"video: playing");
    return true;
}

bool PipelineEngine::EnsureFrameBuf(uint32_t w, uint32_t h)
{
    if (w == 0 || h == 0 || w > vcam::VCamNativeCapW || h > vcam::VCamNativeCapH)
        return false;
    uint64_t need = (uint64_t)h * w * 4;
    if (need == 0 || need > vcam::VCamV2MaxFrameSize) return false;
    if (m_frameW != w || m_frameH != h || m_buf.size() < need) {
        try {
            m_buf.resize((size_t)need);
        } catch (...) {
            return false;
        }
        m_frameW = w;
        m_frameH = h;
    }
    return true;
}

bool PipelineEngine::WriteOne()
{
    if (!m_writerOpen) return false;
    if (m_nativeKnown)
        return m_writer.WriteFrameNative(m_buf.data(), (int)(m_frameW * 4), m_frameW, m_frameH);
    return m_writer.WriteFrame(m_buf.data(), (int)vcam::VCamStride);
}

void PipelineEngine::RecordFrameTime(LARGE_INTEGER t0)
{
    LARGE_INTEGER t1 = {}, freq = {};
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&freq);
    if (freq.QuadPart <= 0) return;
    int64_t us = (t1.QuadPart - t0.QuadPart) * 1000000 / freq.QuadPart;
    if (us < m_frameMinUs) m_frameMinUs = us;
    if (us > m_frameMaxUs) m_frameMaxUs = us;
    m_frameSumUs += us;
    m_frameCount++;
}

bool PipelineEngine::RenderOne(IFrameSource* src, std::wstring& rerr)
{
    LARGE_INTEGER t0 = {};
    QueryPerformanceCounter(&t0);
    uint32_t nw = 0, nh = 0;
    if (src->NativeSize(nw, nh) && nw != 0 && nh != 0 &&
        nw <= vcam::VCamNativeCapW && nh <= vcam::VCamNativeCapH &&
        EnsureFrameBuf(nw, nh)) {
        if (!src->Render(m_buf.data(), (int)(nw * 4), nw, nh, rerr)) return false;
        m_nativeKnown = true;
        RecordFrameTime(t0); // P1.2: время успешного кадра
        return true;
    }
    if (!EnsureFrameBuf(vcam::VCamWidth, vcam::VCamHeight)) {
        rerr = L"frame buffer alloc failed";
        return false;
    }
    if (!src->Render(m_buf.data(), (int)vcam::VCamStride, rerr)) return false;
    m_nativeKnown = false;
    RecordFrameTime(t0); // P1.2: время успешного кадра
    return true;
}

void PipelineEngine::EnterFallback(const std::wstring& reason)
{
    if (m_phase != Phase::Fallback) m_fallbackCount++; // P1.2: только переходы, не повторы
    CloseSource();
    m_phase = Phase::Fallback;
    m_nextAttempt = GetTickCount64() + kFallbackRetryMs;
    Log(L"no signal: " + reason);
}

void PipelineEngine::FlushOrSleep()
{
    if (!m_writer.FlushLast()) Sleep(kFrameMs);
}
