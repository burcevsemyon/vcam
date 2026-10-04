#pragma once

#include <string>

#include "HostPipelineEngine.h"

std::wstring TargetLabel(const SourceConfig& cfg);
std::wstring ActiveStatus(const SourceConfig& cfg, const std::wstring& quality);
std::wstring FallbackStatus(const std::wstring& reason);
void SetActiveStatus(HostPipelineEngine& e);
void CleanupStaleRecordFiles();
void ApplySettingsDiff(HostPipelineEngine& e, HotkeySection& curHotkey,
                       RecordHotkeySection& curRecHotkey);
void CheckBorrowedReturn(HostPipelineEngine& e, const std::wstring& settingsPath);
void SetStatus(const std::wstring& text);
std::wstring GetStatus();
