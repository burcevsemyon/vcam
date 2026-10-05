#pragma once

#include <windows.h>
#include <atlbase.h>

#include <functional>
#include <string>

#include "Settings.h"

// Poll-наблюдатель settings.json (500 ms + debounce 200 ms): «дай текущие
// настройки + уведомь об изменении». Callback вызывается из потока наблюдателя.
class SettingsWatcher {
public:
    using ChangeCallback = std::function<void(const Settings&)>;

    SettingsWatcher();
    ~SettingsWatcher();

    SettingsWatcher(const SettingsWatcher&) = delete;
    SettingsWatcher& operator=(const SettingsWatcher&) = delete;

    // Загружает текущие настройки (Current() доступен сразу) и стартует поток.
    bool Start(const std::wstring& path, ChangeCallback cb);
    void Stop();

    bool Current(Settings& out) const;
    bool HasCurrent() const { return hasCurrent_; }

private:
    static DWORD WINAPI ThreadProc(LPVOID self);
    void PollLoop();

    mutable ATL::CComAutoCriticalSection cs_;
    std::wstring path_;
    ChangeCallback cb_;
    Settings current_;
    bool hasCurrent_ = false;
    ATL::CHandle thread_;
    ATL::CHandle stopEvent_;
    std::string lastRaw_;
};
