#include "CaptureTestPCH.h"
#include "CaptureTestTypes.h"
#include "CaptureTestCommon.h"

struct ScenarioState {
    struct LastCapture {
        int frames = 0;
        int black = 0;
        int unique = 0;
        double meanBrightness = 0;
    };
    LastCapture last;
    double threshold = 40.0;
    int failures = 0;
};

static void ScenarioCallback(void* ctx, const FrameCtx& fc)
{
    ScenarioState* c = (ScenarioState*)ctx;
    int w = (int)fc.s0.dwWidth, h = (int)fc.s0.dwHeight;
    int stride = (fc.s0.sub == MFVideoFormat_NV12) ? w : (int)(fc.s0.dwWidth * 4);
    PixelStats px = ComputePixelStats(fc.pBits, fc.curLen, w, h, stride, c->threshold);
    wchar_t hash[65];
    Sha256Hex(fc.pBits, fc.curLen, hash, _countof(hash));
    static std::vector<std::wstring> s_hashes;
    static int s_black = 0;
    static double s_sumMean = 0;
    static int s_count = 0;
    if (fc.index == 0) { s_hashes.clear(); s_black = 0; s_sumMean = 0; s_count = 0; }
    s_hashes.push_back(hash);
    if (px.mean < c->threshold) ++s_black;
    s_sumMean += px.mean;
    ++s_count;
    c->last.frames = s_count;
    c->last.black = s_black;
    c->last.meanBrightness = s_count ? s_sumMean / s_count : 0;
    std::vector<std::wstring> uniq = s_hashes;
    std::sort(uniq.begin(), uniq.end());
    c->last.unique = (int)(std::unique(uniq.begin(), uniq.end()) - uniq.begin());
}

static std::vector<std::wstring> ReadScriptLines(const wchar_t* path)
{
    std::vector<std::wstring> lines;
    FILE* f = _wfopen(path, L"r, ccs=UTF-8");
    if (!f) {
        LogW(L"scenario: cannot open '%s'", path);
        return lines;
    }
    wchar_t buf[2048];
    while (fgetws(buf, _countof(buf), f)) {
        std::wstring s(buf);
        while (!s.empty() && (s.back() == L'\n' || s.back() == L'\r' || s.back() == L' ' || s.back() == L'\t'))
            s.pop_back();
        while (!s.empty() && (s.front() == L' ' || s.front() == L'\t')) s.erase(s.begin());
        if (!s.empty() && s[0] != L'#') lines.push_back(s);
    }
    fclose(f);
    return lines;
}

int RunScenarioMode(const wchar_t* scriptPath)
{
    std::vector<std::wstring> lines = ReadScriptLines(scriptPath);
    if (lines.empty()) return 2;

    ScenarioState state;
    int lineNo = 0;
    for (const std::wstring& line : lines) {
        ++lineNo;
        std::vector<std::wstring> toks;
        {
            const wchar_t* p = line.c_str();
            while (*p) {
                while (*p == L' ' || *p == L'\t') ++p;
                if (!*p) break;
                const wchar_t* start = p;
                while (*p && *p != L' ' && *p != L'\t') ++p;
                toks.push_back(std::wstring(start, p - start));
            }
        }
        if (toks.empty()) continue;
        const std::wstring& cmd = toks[0];

        if (cmd == L"print") {
            std::wstring rest = line.substr(toks[0].size());
            while (!rest.empty() && (rest.front() == L' ' || rest.front() == L'\t')) rest.erase(rest.begin());
            LogW(L"scenario: %s", rest.c_str());
        } else if (cmd == L"wait" && toks.size() > 1) {
            Sleep((DWORD)_wtoi(toks[1].c_str()));
        } else if (cmd == L"capture") {
            int frames = toks.size() > 1 ? _wtoi(toks[1].c_str()) : 30;
            UINT32 w = toks.size() > 2 ? (UINT32)_wtoi(toks[2].c_str()) : 1280;
            UINT32 h = toks.size() > 3 ? (UINT32)_wtoi(toks[3].c_str()) : 720;
            int interval = toks.size() > 4 ? _wtoi(toks[4].c_str()) : 100;
            CapOptions opt;
            opt.numFrames = frames;
            opt.reqW = w;
            opt.reqH = h;
            opt.intervalMs = interval;
            opt.deadlineMs = (ULONGLONG)(frames * (std::max)(interval, 1) + 30000);
            int rc = RunCaptureSession(opt, ScenarioCallback, &state, nullptr, nullptr);
            if (rc != 0) {
                LogW(L"scenario line %d: capture failed rc=%d", lineNo, rc);
                ++state.failures;
            } else {
                LogW(L"scenario line %d: captured %d frames (black=%d unique=%d mean=%.1f)",
                     lineNo, state.last.frames, state.last.black, state.last.unique, state.last.meanBrightness);
            }
        } else if (cmd == L"assert-frames" && toks.size() > 1) {
            int want = _wtoi(toks[1].c_str());
            bool ok = (state.last.frames == want);
            if (!ok) ++state.failures;
            LogW(L"scenario line %d: assert-frames %d — %s (got %d)",
                 lineNo, want, ok ? L"OK" : L"FAIL", state.last.frames);
        } else if (cmd == L"assert-black" && toks.size() > 1) {
            int maxBlack = _wtoi(toks[1].c_str());
            bool ok = (state.last.black <= maxBlack);
            if (!ok) ++state.failures;
            LogW(L"scenario line %d: assert-black <=%d — %s (got %d)",
                 lineNo, maxBlack, ok ? L"OK" : L"FAIL", state.last.black);
        } else if (cmd == L"assert-unique" && toks.size() > 1) {
            int want = _wtoi(toks[1].c_str());
            bool ok = (state.last.unique == want);
            if (!ok) ++state.failures;
            LogW(L"scenario line %d: assert-unique %d — %s (got %d)",
                 lineNo, want, ok ? L"OK" : L"FAIL", state.last.unique);
        } else if (cmd == L"assert-unique-gt" && toks.size() > 1) {
            int want = _wtoi(toks[1].c_str());
            bool ok = (state.last.unique > want);
            if (!ok) ++state.failures;
            LogW(L"scenario line %d: assert-unique-gt %d — %s (got %d)",
                 lineNo, want, ok ? L"OK" : L"FAIL", state.last.unique);
        } else if (cmd == L"assert-mean-gt" && toks.size() > 1) {
            double want = _wtof(toks[1].c_str());
            bool ok = (state.last.meanBrightness > want);
            if (!ok) ++state.failures;
            LogW(L"scenario line %d: assert-mean-gt %.1f — %s (got %.1f)",
                 lineNo, want, ok ? L"OK" : L"FAIL", state.last.meanBrightness);
        } else if (cmd == L"switch-wait" && toks.size() > 2) {
            const wchar_t* path = toks[1].c_str();
            int timeoutMs = _wtoi(toks[2].c_str());
            ULONGLONG mt0 = FileMtimeMs(path);
            bool changed = false;
            ULONGLONG deadline = GetTickCount64() + (ULONGLONG)timeoutMs;
            while (GetTickCount64() < deadline) {
                ULONGLONG mt = FileMtimeMs(path);
                if (mt != 0 && mt != mt0) { changed = true; break; }
                Sleep(100);
            }
            LogW(L"scenario line %d: switch-wait — %s", lineNo, changed ? L"CHANGED" : L"TIMEOUT");
            if (!changed) ++state.failures;
        } else if (cmd == L"stop") {
            break;
        } else {
            LogW(L"scenario line %d: unknown command '%s'", lineNo, cmd.c_str());
            ++state.failures;
        }
    }

    const wchar_t* verdict = (state.failures == 0) ? L"OK" : L"FAIL";
    LogW(L"--- scenario summary: lines=%d failures=%d verdict=%s", lineNo, state.failures, verdict);
    return state.failures == 0 ? 0 : 1;
}