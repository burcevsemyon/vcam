#include "SettingsWatcher.h"

#include <atlbase.h>
#include <cstring>

#include "CriticalSectionGuard.h"

namespace {

bool ReadUtf8File(const std::wstring& path, std::string& out)
{
    HANDLE raw = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return false;
    ATL::CHandle h(raw);
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(h, &sz) || sz.QuadPart <= 0 || sz.QuadPart > 1000000) return false;
    out.resize((size_t)sz.QuadPart);
    DWORD read = 0;
    BOOL ok = ReadFile(h, &out[0], (DWORD)out.size(), &read, nullptr);
    return ok && read == out.size();
}

} // namespace

SettingsWatcher::SettingsWatcher()
{
    InitializeCriticalSection(&cs_);
    csInit_ = true;
}

SettingsWatcher::~SettingsWatcher()
{
    Stop();
    if (csInit_) { DeleteCriticalSection(&cs_); csInit_ = false; }
}

bool SettingsWatcher::Start(const std::wstring& path, ChangeCallback cb)
{
    if (thread_) return false;
    path_ = path;
    cb_ = std::move(cb);
    if (path_.empty()) return false;

    Settings s;
    bool loaded = s.Load(path_);
    std::string raw;
    if (!ReadUtf8File(path_, raw)) raw.clear();

    {
        vcam::CsGuard guard(&cs_);
        current_ = s;
        hasCurrent_ = loaded;
        lastRaw_ = raw;
    }

    stopEvent_.Attach(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stopEvent_) return false;
    thread_.Attach(CreateThread(nullptr, 0, ThreadProc, this, 0, nullptr));
    if (!thread_) {
        stopEvent_.Close();
        return false;
    }
    return true;
}

void SettingsWatcher::Stop()
{
    if (stopEvent_) SetEvent(stopEvent_);
    if (thread_) {
        WaitForSingleObject(thread_, 2000);
        thread_.Close();
    }
    stopEvent_.Close();
    cb_ = nullptr;
}

bool SettingsWatcher::Current(Settings& out) const
{
    vcam::CsGuard guard(&cs_);
    out = current_;
    bool ok = hasCurrent_;
    return ok;
}

DWORD WINAPI SettingsWatcher::ThreadProc(LPVOID self)
{
    static_cast<SettingsWatcher*>(self)->PollLoop();
    return 0;
}

void SettingsWatcher::PollLoop()
{
    for (;;) {
        if (WaitForSingleObject(stopEvent_, 500) != WAIT_TIMEOUT) break;

        std::string raw;
        if (!ReadUtf8File(path_, raw)) {
            // Файл исчез: сбрасываем базу, чтобы повторное появление сработало.
            {
                vcam::CsGuard guard(&cs_);
                lastRaw_.clear();
            }
            continue;
        }

        bool same = false;
        {
            vcam::CsGuard guard(&cs_);
            same = (raw == lastRaw_);
        }
        if (same) continue;

        Sleep(200); // debounce: let the writer finish
        if (!ReadUtf8File(path_, raw)) continue;

        {
            vcam::CsGuard guard(&cs_);
            same = (raw == lastRaw_);
            if (!same) lastRaw_ = raw;
        }
        if (same) continue;

        Settings s;
        if (!s.Load(path_)) continue;

        bool changed;
        ChangeCallback cb;
        {
            vcam::CsGuard guard(&cs_);
            changed = (!hasCurrent_ || s != current_);
            if (changed) { current_ = s; hasCurrent_ = true; }
            cb = cb_;
        }

        if (changed && cb) {
            try {
                cb(s);
            } catch (...) {
                // исключение наблюдателя не должно убивать поток
            }
        }
    }
}
