#pragma once

#include <string>

std::wstring RecordStatePath();
std::wstring RecordCommandPath();
std::wstring DefaultRecordPath();
std::string EscapeJsonUtf8(const std::wstring& w);
void WriteRecordState(const std::wstring& path);
void ClearRecordState();
bool ScanJsonString(const std::string& json, const char* key, std::wstring& value);
bool ScanJsonInt(const std::string& json, const char* key, long long& value);
bool ReadSmallFile(const std::wstring& path, std::string& out);
bool TryReadRecordState(std::wstring& path, long long& started);
bool WriteRecordCommand(const std::wstring& cmd, const std::wstring& path);
bool ConsumeRecordCommand(std::wstring& cmd, std::wstring& path);
