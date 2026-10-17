#pragma once

#include <string>

#include "HostPipelineEngine.h"

std::wstring TargetLabel(const SourceConfig& cfg);
std::wstring ActiveStatus(const SourceConfig& cfg, const std::wstring& quality);
std::wstring FallbackStatus(const std::wstring& reason);
void SetActiveStatus(HostPipelineEngine& e);
void CleanupStaleRecordFiles();
// P2.2: crash-маркер + последний шаг (переживают рестарт).
void CheckPreviousRunCrash(); // на старте: маркер прошлого падения + свежий "starting"
void UpdateRunStep(const std::wstring& step); // при смене фазы (только если изменился)
void ClearRunState(); // при чистом выходе: удалить файл
void ApplySettingsDiff(HostPipelineEngine& e, HotkeySection& curHotkey,
                       RecordHotkeySection& curRecHotkey,
                       VideoHotkeySection& curVideoHotkey,
                       SourceSwitchHotkeySection& curSrcStaticHotkey,
                       SourceSwitchHotkeySection& curSrcVideoHotkey,
                       SourceSwitchHotkeySection& curSrcCameraHotkey);
void CheckBorrowedReturn(HostPipelineEngine& e, const std::wstring& settingsPath);
void SetStatus(const std::wstring& text);
std::wstring GetStatus();
