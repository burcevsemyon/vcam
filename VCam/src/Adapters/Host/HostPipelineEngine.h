#pragma once

#include "PipelineEngine.h"
#include "Mp4Recorder.h"

void HostLog(const std::wstring& msg);

class HostPipelineEngine : public PipelineEngine {
public:
    HostPipelineEngine();
    ~HostPipelineEngine();

    void PostProcessFrame(uint8_t* bgrx, int stride, uint32_t w, uint32_t h) override;

    bool StartRecording(const std::wstring& requested);
    void StopRecording(const std::wstring& reason);
    bool IsRecording() const { return m_rec.IsOpen(); }
    // P1.2: кадры записи для runtime-метрик (0, если не идёт).
    uint64_t RecFramesWritten() const { return m_rec.IsOpen() ? m_rec.FramesWritten() : 0ULL; }
    uint64_t RecFramesDropped() const { return m_rec.IsOpen() ? m_rec.FramesDropped() : 0ULL; }

protected:
    void Log(const std::wstring& msg) override;

private:
    Mp4Recorder m_rec;
    bool m_recErrLogged = false;
    uint64_t m_recLastDropped = 0;
    void RecordEtherFrame();
};
