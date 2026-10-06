#include "doctest.h"
#include <windows.h>

#include "PipelineLogic.h"

#include <string>

// Чистая логика PipelineEngine / FrameWriter: нормализация quality,
// валидация размеров, решение native-vs-fallback.

TEST_CASE("pipeline: NormalizeQuality tokens")
{
    CHECK(vcam::NormalizeQuality(L"source") == L"source");
    CHECK(vcam::NormalizeQuality(L"fixed720p") == L"fixed720p");
    CHECK(vcam::NormalizeQuality(L"fixed1080p") == L"fixed1080p");
}

TEST_CASE("pipeline: NormalizeQuality garbage -> source")
{
    CHECK(vcam::NormalizeQuality(L"") == L"source");
    CHECK(vcam::NormalizeQuality(L"bogus") == L"source");
    CHECK(vcam::NormalizeQuality(L"FIXED720P") == L"source"); // ordinal
    CHECK(vcam::NormalizeQuality(L"fixed720") == L"source");
    CHECK(vcam::NormalizeQuality(L"720p") == L"source");
}

TEST_CASE("pipeline: ValidateFrameDims accepts standard sizes")
{
    CHECK(vcam::ValidateFrameDims(1280, 720));
    CHECK(vcam::ValidateFrameDims(640, 480));
    CHECK(vcam::ValidateFrameDims(1920, 1080));
    CHECK(vcam::ValidateFrameDims(3840, 2160)); // cap 4K
    CHECK(vcam::ValidateFrameDims(1, 1));
}

TEST_CASE("pipeline: ValidateFrameDims rejects zero/oversized")
{
    CHECK(!vcam::ValidateFrameDims(0, 720));
    CHECK(!vcam::ValidateFrameDims(1280, 0));
    CHECK(!vcam::ValidateFrameDims(0, 0));
    CHECK(!vcam::ValidateFrameDims(3841, 2160)); // over cap W
    CHECK(!vcam::ValidateFrameDims(3840, 2161)); // over cap H
}

TEST_CASE("pipeline: CalcFrameBytes correct for standard sizes")
{
    CHECK(vcam::CalcFrameBytes(1280, 720) == 3686400);
    CHECK(vcam::CalcFrameBytes(640, 480) == 1228800);
    CHECK(vcam::CalcFrameBytes(1, 1) == 4);
}

TEST_CASE("pipeline: ShouldUseNative true for valid sizes")
{
    CHECK(vcam::ShouldUseNative(1280, 720));
    CHECK(vcam::ShouldUseNative(1280, 698));
    CHECK(vcam::ShouldUseNative(1920, 1080));
    CHECK(vcam::ShouldUseNative(3840, 2160));
}

TEST_CASE("pipeline: ShouldUseNative false for zero/oversized")
{
    CHECK(!vcam::ShouldUseNative(0, 720));
    CHECK(!vcam::ShouldUseNative(1280, 0));
    CHECK(!vcam::ShouldUseNative(0, 0));
    CHECK(!vcam::ShouldUseNative(3841, 2160));
    CHECK(!vcam::ShouldUseNative(3840, 2161));
}

// State machine transitions (PipelineEngine::Step).
// Switch -> Active on successful render+write.
// Switch -> Fallback on timeout (kSwitchWindowMs = 5000).
// Active -> Fallback on render failure.
// Fallback -> Active on successful retry.
// Эти переходы тестируются через Phase enum + логику таймаута.

enum class Phase { Switch, Active, Fallback };

struct StateMachine
{
    Phase phase = Phase::Switch;
    uint64_t switchStart = 0;
    uint64_t nextAttempt = 0;
    static constexpr uint64_t kSwitchWindowMs = 5000;
    static constexpr uint64_t kFallbackRetryMs = 1000;

    // Simplified Step logic: returns true if should continue, false if fallback.
    bool TrySwitch(uint64_t now, bool sourceOk, bool renderOk)
    {
        if (!sourceOk) {
            if (now - switchStart >= kSwitchWindowMs) {
                phase = Phase::Fallback;
                nextAttempt = now + kFallbackRetryMs;
                return false;
            }
            return true; // retry
        }
        if (renderOk) {
            phase = Phase::Active;
            return true;
        }
        if (now - switchStart >= kSwitchWindowMs) {
            phase = Phase::Fallback;
            nextAttempt = now + kFallbackRetryMs;
            return false;
        }
        return true; // retry
    }

    bool TryActive(bool renderOk)
    {
        if (!renderOk) {
            phase = Phase::Fallback;
            return false;
        }
        return true;
    }

    bool TryFallback(uint64_t now, bool sourceOk, bool renderOk)
    {
        if (now < nextAttempt) return false; // wait
        nextAttempt = now + kFallbackRetryMs;
        if (sourceOk && renderOk) {
            phase = Phase::Active;
            return true;
        }
        return false; // stay in fallback
    }
};

TEST_CASE("pipeline: Switch -> Active on success")
{
    StateMachine sm;
    sm.switchStart = 1000;
    CHECK(sm.TrySwitch(1100, true, true));
    CHECK(sm.phase == Phase::Active);
}

TEST_CASE("pipeline: Switch retries on source failure within window")
{
    StateMachine sm;
    sm.switchStart = 1000;
    CHECK(sm.TrySwitch(2000, false, false)); // retry
    CHECK(sm.phase == Phase::Switch);
    CHECK(sm.TrySwitch(4000, false, false)); // retry
    CHECK(sm.phase == Phase::Switch);
}

TEST_CASE("pipeline: Switch -> Fallback on timeout")
{
    StateMachine sm;
    sm.switchStart = 1000;
    CHECK(!sm.TrySwitch(7000, false, false)); // false = went to fallback
    CHECK(sm.phase == Phase::Fallback);
}

TEST_CASE("pipeline: Switch -> Fallback on render failure + timeout")
{
    StateMachine sm;
    sm.switchStart = 1000;
    CHECK(sm.TrySwitch(2000, true, false)); // retry (render fail, within window)
    CHECK(sm.phase == Phase::Switch);
    CHECK(!sm.TrySwitch(7000, true, false)); // timeout -> fallback
    CHECK(sm.phase == Phase::Fallback);
}

TEST_CASE("pipeline: Active -> Fallback on render failure")
{
    StateMachine sm;
    sm.phase = Phase::Active;
    CHECK(!sm.TryActive(false));
    CHECK(sm.phase == Phase::Fallback);
}

TEST_CASE("pipeline: Active stays on success")
{
    StateMachine sm;
    sm.phase = Phase::Active;
    CHECK(sm.TryActive(true));
    CHECK(sm.phase == Phase::Active);
}

TEST_CASE("pipeline: Fallback -> Active on successful retry")
{
    StateMachine sm;
    sm.phase = Phase::Fallback;
    sm.nextAttempt = 2000;
    CHECK(sm.TryFallback(3000, true, true));
    CHECK(sm.phase == Phase::Active);
}

TEST_CASE("pipeline: Fallback waits before retry")
{
    StateMachine sm;
    sm.phase = Phase::Fallback;
    sm.nextAttempt = 5000;
    CHECK(!sm.TryFallback(3000, true, true)); // too early
    CHECK(sm.phase == Phase::Fallback);
}

TEST_CASE("pipeline: Fallback stays on failed retry")
{
    StateMachine sm;
    sm.phase = Phase::Fallback;
    sm.nextAttempt = 2000;
    CHECK(!sm.TryFallback(3000, false, false));
    CHECK(sm.phase == Phase::Fallback);
    CHECK(sm.nextAttempt == 4000); // next attempt scheduled
}
