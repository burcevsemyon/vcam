#pragma once

#include "ProducerApi.h"
#include "FrameWriter.h"
#include "Settings.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

class PipelineEngine {
public:
    enum class Phase { Switch, Active, Fallback };

    PipelineEngine();
    ~PipelineEngine();

    PipelineEngine(const PipelineEngine&) = delete;
    PipelineEngine& operator=(const PipelineEngine&) = delete;

    bool Open();
    void Close();

    DWORD Step();

    void SetTarget(const SourceConfig& want, const std::wstring& quality);

    bool IsWriterOpen() const { return m_writerOpen; }
    const std::wstring& WriterErr() const { return m_writerErr; }
    const std::wstring& SectionOpenedAs() const { return m_writer.SectionOpenedAs(); }
    const std::wstring& V2SectionOpenedAs() const { return m_writer.V2SectionOpenedAs(); }
    bool IsV2Open() const { return m_writer.IsV2Open(); }
    const SourceConfig& Target() const { return m_target; }
    bool HasTarget() const { return m_hasTarget; }
    const std::wstring& Quality() const { return m_quality; }
    std::unique_ptr<IFrameSource>& Src() { return m_src; }
    Phase& PhaseRef() { return m_phase; }

    virtual void PostProcessFrame(uint8_t* bgrx, int stride, uint32_t w, uint32_t h) {}

protected:
    virtual void Log(const std::wstring& msg) = 0;

    FrameWriter& Writer() { return m_writer; }
    std::vector<uint8_t>& Buf() { return m_buf; }
    uint32_t& FrameW() { return m_frameW; }
    uint32_t& FrameH() { return m_frameH; }
    bool& NativeKnown() { return m_nativeKnown; }
    ULONGLONG& NextAttempt() { return m_nextAttempt; }
    ULONGLONG& SwitchStart() { return m_switchStart; }

private:
    FrameWriter m_writer;
    bool m_writerOpen = false;
    std::wstring m_writerErr;
    std::unique_ptr<IFrameSource> m_src;
    SourceConfig m_target;
    bool m_hasTarget = false;
    std::wstring m_quality = L"source";
    Phase m_phase = Phase::Switch;
    ULONGLONG m_switchStart = 0;
    ULONGLONG m_nextAttempt = 0;
    std::vector<uint8_t> m_buf;
    uint32_t m_frameW = vcam::VCamWidth;
    uint32_t m_frameH = vcam::VCamHeight;
    bool m_nativeKnown = false;

    void CloseSource();
    bool EnsureFrameBuf(uint32_t w, uint32_t h);
    bool WriteOne();
    bool RenderOne(IFrameSource* src, std::wstring& rerr);
    void EnterFallback(const std::wstring& reason);
    void FlushOrSleep();
};
