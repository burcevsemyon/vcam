#pragma once

#include "PipelineEngine.h"

void CliLog(const std::wstring& msg);

class CliPipelineEngine : public PipelineEngine {
public:
    CliPipelineEngine();
    ~CliPipelineEngine();

protected:
    void Log(const std::wstring& msg) override;
};
