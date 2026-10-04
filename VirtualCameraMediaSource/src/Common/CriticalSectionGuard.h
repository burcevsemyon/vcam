#pragma once
#include <windows.h>

// Header-only RAII-гард для CRITICAL_SECTION (общий для всех проектов:
// MediaSource.vcxproj не линкует ProducerCore, поэтому только заголовок,
// без .cpp). Все проекты уже включают ..\Common — правки .vcxproj не нужны.
//
// Семантика 1:1 с ручной парой Enter/Leave: ctor = Enter, dtor = Leave.
// Ctor принимает const CRITICAL_SECTION* (const_cast внутри), чтобы
// const-методы (Ended, IsRunning и т.п.) не делали const_cast на стороне вызова.
//
// ВАЖНО: не расширять удержание на блокирующие вызовы. Если код делал Leave
// перед блокирующим вызовом (Wait/Sleep/Acquire/callback) и Enter после —
// гард должен жить в скоупе { } ровно на месте старой пары.

namespace vcam {

class CsGuard {
public:
    explicit CsGuard(const CRITICAL_SECTION* cs)
        : cs_(const_cast<CRITICAL_SECTION*>(cs))
    {
        EnterCriticalSection(cs_);
    }
    ~CsGuard() { LeaveCriticalSection(cs_); }

    CsGuard(const CsGuard&) = delete;
    CsGuard& operator=(const CsGuard&) = delete;

private:
    CRITICAL_SECTION* cs_;
};

} // namespace vcam
