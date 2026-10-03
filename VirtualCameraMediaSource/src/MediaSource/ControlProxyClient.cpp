// Прокси-клиент управляющего канала (см. ControlProxyClient.h).
// Протокол — src/ProducerCore/ControlServer.h (контракт Sub 1).

#include "ControlProxyClient.h"

#include <atlbase.h>
#include <mferror.h> // MF_E_* для единообразия (не используется напрямую)

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "WinUtil.h"

#pragma comment(lib, "strmiids.lib") // IID_IAMVideoProcAmp/IAMCameraControl

// TEMP DIAGNOSTIC - определён в dllmain.cpp (в Release no-op, как весь файл).
void VCamDiagLog(const wchar_t* fmt, ...);

#ifndef E_PROP_ID_UNSUPPORTED
#define E_PROP_ID_UNSUPPORTED ((HRESULT)0x80070490L) // vfwmsgs.h
#endif

namespace {

constexpr DWORD kProxyTimeoutMs = 3000; // один синхронный вызов, не более
constexpr size_t kMaxLine = 256 * 1024;
constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\VCamControl.v1";

using vcam::WinErr;

// --- Транспорт: та же семантика, что ControlClientRequest (Sub 1) ---

bool ProxyTransact(const std::string& request, std::string& response,
                   std::wstring& err)
{
    response.clear();
    ULONGLONG openDeadline = GetTickCount64() + kProxyTimeoutMs;
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD openErr = 0;
    for (;;) {
        h = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                        OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE)
            break;
        openErr = GetLastError();
        if (openErr == ERROR_PIPE_BUSY) {
            ULONGLONG now = GetTickCount64();
            DWORD left = (now >= openDeadline) ? 0 : (DWORD)(openDeadline - now);
            if (left == 0 || !WaitNamedPipeW(kPipeName, left)) {
                err = L"control pipe busy/timeout";
                return false;
            }
            continue;
        }
        if (openErr != ERROR_FILE_NOT_FOUND)
            break;
        if (GetTickCount64() >= openDeadline)
            break;
        Sleep(20);
    }
    if (h == INVALID_HANDLE_VALUE) {
        err = L"no control server: " + WinErr(openErr);
        return false;
    }
    ATL::CHandle pipe(h);

    std::string line = request;
    line += '\n';
    DWORD written = 0;
    if (!WriteFile(pipe, line.data(), (DWORD)line.size(), &written, nullptr) ||
        written != line.size()) {
        err = L"pipe write: " + WinErr(GetLastError());
        return false;
    }

    std::string acc;
    ULONGLONG deadline = GetTickCount64() + kProxyTimeoutMs;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            err = L"control server reply timeout";
            return false;
        }
        DWORD wait = (DWORD)(deadline - now);
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr)) {
            err = L"server closed: " + WinErr(GetLastError());
            return false;
        }
        if (avail == 0) {
            Sleep(wait > 20 ? 20 : wait);
            continue;
        }
        char chunk[4096];
        DWORD read = 0;
        if (!ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) ||
            read == 0) {
            err = L"pipe read: " + WinErr(GetLastError());
            return false;
        }
        acc.append(chunk, read);
        if (acc.size() > kMaxLine) {
            err = L"reply too long";
            return false;
        }
        size_t pos = acc.find('\n');
        if (pos != std::string::npos) {
            response = acc.substr(0, pos);
            if (!response.empty() && response.back() == '\r')
                response.pop_back();
            return true;
        }
    }
}

// --- Мини-DOM для машинных ответов сервера ---

struct JVal {
    char type = 0; // 'n' число, 'b' bool, 's' строка, 'o' объект, 'a' массив
    long num = 0;
    bool boolean = false;
    std::string str;
    std::vector<std::pair<std::string, JVal>> obj;
    std::vector<JVal> arr;
};

struct JParser {
    const char* s = nullptr;
    size_t n = 0;
    size_t p = 0;
    bool fail = false;

    void Ws()
    {
        while (p < n &&
               (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n'))
            p++;
    }
    bool ParseString(std::string& out)
    {
        out.clear();
        if (p >= n || s[p] != '"') {
            fail = true;
            return false;
        }
        p++;
        while (p < n) {
            char c = s[p];
            if (c == '"') {
                p++;
                return true;
            }
            if (c == '\\') {
                p++;
                if (p >= n) {
                    fail = true;
                    return false;
                }
                char e = s[p];
                switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': { // \uXXXX -> UTF-8 (ответы ASCII, но парсим честно)
                    if (p + 4 >= n) {
                        fail = true;
                        return false;
                    }
                    auto hex = [&](size_t k) -> int {
                        char hh = s[k];
                        if (hh >= '0' && hh <= '9')
                            return hh - '0';
                        if (hh >= 'a' && hh <= 'f')
                            return hh - 'a' + 10;
                        if (hh >= 'A' && hh <= 'F')
                            return hh - 'A' + 10;
                        return -1;
                    };
                    unsigned cp = 0;
                    for (int k = 1; k <= 4; k++) {
                        int hh = hex(p + (size_t)k);
                        if (hh < 0) {
                            fail = true;
                            return false;
                        }
                        cp = cp * 16 + (unsigned)hh;
                    }
                    p += 4;
                    if (cp < 0x80)
                        out += (char)cp;
                    else if (cp < 0x800) {
                        out += (char)(0xC0 | (cp >> 6));
                        out += (char)(0x80 | (cp & 0x3F));
                    } else {
                        out += (char)(0xE0 | (cp >> 12));
                        out += (char)(0x80 | ((cp >> 6) & 0x3F));
                        out += (char)(0x80 | (cp & 0x3F));
                    }
                    break;
                }
                default: fail = true; return false;
                }
                p++;
                continue;
            }
            if ((unsigned char)c < 0x20) {
                fail = true;
                return false;
            }
            out += c;
            p++;
        }
        fail = true;
        return false;
    }
    bool ParseNumber(long& out)
    {
        size_t start = p;
        if (p < n && (s[p] == '-' || s[p] == '+'))
            p++;
        size_t digits = p;
        while (p < n && s[p] >= '0' && s[p] <= '9')
            p++;
        if (p == digits) {
            fail = true;
            return false;
        }
        if (p < n && (s[p] == '.' || s[p] == 'e' || s[p] == 'E')) {
            fail = true;
            return false; // протокол — только целые
        }
        out = strtol(std::string(s + start, p - start).c_str(), nullptr, 10);
        return true;
    }
    bool ParseValue(JVal& out);
    bool ParseObject(JVal& out)
    {
        out.type = 'o';
        p++; // '{'
        Ws();
        if (p < n && s[p] == '}') {
            p++;
            return true;
        }
        for (;;) {
            Ws();
            std::string key;
            if (!ParseString(key))
                return false;
            Ws();
            if (p >= n || s[p] != ':') {
                fail = true;
                return false;
            }
            p++;
            JVal v;
            if (!ParseValue(v))
                return false;
            out.obj.emplace_back(key, std::move(v));
            Ws();
            if (p >= n) {
                fail = true;
                return false;
            }
            if (s[p] == ',') {
                p++;
                continue;
            }
            if (s[p] == '}') {
                p++;
                return true;
            }
            fail = true;
            return false;
        }
    }
    bool ParseArray(JVal& out)
    {
        out.type = 'a';
        p++; // '['
        Ws();
        if (p < n && s[p] == ']') {
            p++;
            return true;
        }
        for (;;) {
            JVal v;
            if (!ParseValue(v))
                return false;
            out.arr.push_back(std::move(v));
            Ws();
            if (p >= n) {
                fail = true;
                return false;
            }
            if (s[p] == ',') {
                p++;
                continue;
            }
            if (s[p] == ']') {
                p++;
                return true;
            }
            fail = true;
            return false;
        }
    }
};

bool JParser::ParseValue(JVal& out)
{
    Ws();
    if (p >= n) {
        fail = true;
        return false;
    }
    char c = s[p];
    if (c == '{')
        return ParseObject(out);
    if (c == '[')
        return ParseArray(out);
    if (c == '"') {
        out.type = 's';
        return ParseString(out.str);
    }
    if (c == 't' && p + 4 <= n && memcmp(s + p, "true", 4) == 0) {
        out.type = 'b';
        out.boolean = true;
        p += 4;
        return true;
    }
    if (c == 'f' && p + 5 <= n && memcmp(s + p, "false", 5) == 0) {
        out.type = 'b';
        out.boolean = false;
        p += 5;
        return true;
    }
    if (c == '-' || c == '+' || (c >= '0' && c <= '9')) {
        out.type = 'n';
        return ParseNumber(out.num);
    }
    fail = true;
    return false;
}

const JVal* FindKey(const JVal& obj, const char* key)
{
    if (obj.type != 'o')
        return nullptr;
    for (const auto& kv : obj.obj)
        if (kv.first == key)
            return &kv.second;
    return nullptr;
}

// Верх ответа: ok(bool) + hr(число). false = битый ответ.
bool ParseOkHr(const std::string& resp, bool& ok, long& hr)
{
    JParser pr;
    pr.s = resp.c_str();
    pr.n = resp.size();
    JVal root;
    if (!pr.ParseValue(root) || pr.fail || root.type != 'o')
        return false;
    const JVal* jok = FindKey(root, "ok");
    const JVal* jhr = FindKey(root, "hr");
    if (!jok || jok->type != 'b' || !jhr || jhr->type != 'n')
        return false;
    ok = jok->boolean;
    hr = jhr->num;
    return true;
}

// ok-ответ get/set: cur + flags числами.
bool ParseCurFlags(const std::string& resp, long& cur, long& flags)
{
    JParser pr;
    pr.s = resp.c_str();
    pr.n = resp.size();
    JVal root;
    if (!pr.ParseValue(root) || pr.fail || root.type != 'o')
        return false;
    const JVal* jok = FindKey(root, "ok");
    const JVal* jcur = FindKey(root, "cur");
    const JVal* jfl = FindKey(root, "flags");
    if (!jok || jok->type != 'b' || !jok->boolean)
        return false;
    if (!jcur || jcur->type != 'n' || !jfl || jfl->type != 'n')
        return false;
    cur = jcur->num;
    flags = jfl->num;
    return true;
}

// list-ответ: ищем запись domain/id, забираем диапазон. supported=false
// или отсутствие записи = «ручки нет».
bool ParseListEntry(const std::string& resp, const char* domain, long id,
                    long& mn, long& mx, long& step, long& def, long& caps)
{
    JParser pr;
    pr.s = resp.c_str();
    pr.n = resp.size();
    JVal root;
    if (!pr.ParseValue(root) || pr.fail || root.type != 'o')
        return false;
    const JVal* jok = FindKey(root, "ok");
    const JVal* jctl = FindKey(root, "controls");
    if (!jok || jok->type != 'b' || !jok->boolean)
        return false;
    if (!jctl || jctl->type != 'a')
        return false;
    for (const JVal& e : jctl->arr) {
        const JVal* d = FindKey(e, "domain");
        const JVal* i = FindKey(e, "id");
        if (!d || d->type != 's' || !i || i->type != 'n')
            continue;
        if (d->str != domain || i->num != id)
            continue;
        const JVal* sup = FindKey(e, "supported");
        if (!sup || sup->type != 'b' || !sup->boolean)
            return false; // запись есть, но драйвер не даёт — ручки нет
        const JVal* jmn = FindKey(e, "min");
        const JVal* jmx = FindKey(e, "max");
        const JVal* jst = FindKey(e, "step");
        const JVal* jdf = FindKey(e, "def");
        const JVal* jcp = FindKey(e, "caps");
        if (!jmn || jmn->type != 'n' || !jmx || jmx->type != 'n' ||
            !jst || jst->type != 'n' || !jdf || jdf->type != 'n' || !jcp ||
            jcp->type != 'n')
            return false;
        mn = jmn->num;
        mx = jmx->num;
        step = jst->num;
        def = jdf->num;
        caps = jcp->num;
        return true;
    }
    return false; // записи нет (устройство закрыто / controls:[])
}

const char* DomainName(bool isProcAmp)
{
    return isProcAmp ? "procamp" : "camera";
}

} // namespace

bool VCamProxyIsProcAmpProp(long prop)
{
    return (prop >= 0 && prop <= 5) || (prop >= 7 && prop <= 9);
}

bool VCamProxyIsCameraProp(long prop)
{
    return prop >= 0 && prop <= 6;
}

HRESULT VCamProxyGetRange(bool isProcAmp, long prop, long* pMin, long* pMax,
                          long* pStep, long* pDef, long* pCaps)
{
    if (!pMin || !pMax || !pStep || !pDef || !pCaps)
        return E_POINTER;
    if ((isProcAmp && !VCamProxyIsProcAmpProp(prop)) ||
        (!isProcAmp && !VCamProxyIsCameraProp(prop))) {
        VCamDiagLog(L"CtlProxy.GetRange %hs:%d -> E_PROP_ID_UNSUPPORTED (bad id)",
                    DomainName(isProcAmp), prop);
        return E_PROP_ID_UNSUPPORTED;
    }
    std::string resp;
    std::wstring err;
    if (!ProxyTransact("{\"op\":\"list\"}", resp, err)) {
        VCamDiagLog(L"CtlProxy.GetRange %hs:%d -> E_PROP_ID_UNSUPPORTED (%s)",
                    DomainName(isProcAmp), prop, err.c_str());
        return E_PROP_ID_UNSUPPORTED;
    }
    long mn = 0, mx = 0, step = 0, def = 0, caps = 0;
    if (!ParseListEntry(resp, DomainName(isProcAmp), prop, mn, mx, step, def,
                        caps)) {
        VCamDiagLog(L"CtlProxy.GetRange %hs:%d -> E_PROP_ID_UNSUPPORTED",
                    DomainName(isProcAmp), prop);
        return E_PROP_ID_UNSUPPORTED;
    }
    *pMin = mn;
    *pMax = mx;
    *pStep = step;
    *pDef = def;
    *pCaps = caps;
    VCamDiagLog(L"CtlProxy.GetRange %hs:%d -> ok [%d,%d] step=%d def=%d caps=%d",
                DomainName(isProcAmp), prop, mn, mx, step, def, caps);
    return S_OK;
}

HRESULT VCamProxyGet(bool isProcAmp, long prop, long* pVal, long* pFlags)
{
    if (!pVal || !pFlags)
        return E_POINTER;
    if ((isProcAmp && !VCamProxyIsProcAmpProp(prop)) ||
        (!isProcAmp && !VCamProxyIsCameraProp(prop))) {
        VCamDiagLog(L"CtlProxy.Get %hs:%d -> E_PROP_ID_UNSUPPORTED (bad id)",
                    DomainName(isProcAmp), prop);
        return E_PROP_ID_UNSUPPORTED;
    }
    char req[128];
    snprintf(req, sizeof(req), "{\"op\":\"get\",\"domain\":\"%s\",\"id\":%d}",
             DomainName(isProcAmp), prop);
    std::string resp;
    std::wstring err;
    if (!ProxyTransact(req, resp, err)) {
        VCamDiagLog(L"CtlProxy.Get %hs:%d -> E_PROP_ID_UNSUPPORTED (%s)",
                    DomainName(isProcAmp), prop, err.c_str());
        return E_PROP_ID_UNSUPPORTED;
    }
    long cur = 0, flags = 0;
    if (!ParseCurFlags(resp, cur, flags)) {
        VCamDiagLog(L"CtlProxy.Get %hs:%d -> E_PROP_ID_UNSUPPORTED",
                    DomainName(isProcAmp), prop);
        return E_PROP_ID_UNSUPPORTED;
    }
    *pVal = cur;
    *pFlags = flags;
    VCamDiagLog(L"CtlProxy.Get %hs:%d -> ok val=%d flags=%d",
                DomainName(isProcAmp), prop, cur, flags);
    return S_OK;
}

HRESULT VCamProxySet(bool isProcAmp, long prop, long val, long flags)
{
    if ((isProcAmp && !VCamProxyIsProcAmpProp(prop)) ||
        (!isProcAmp && !VCamProxyIsCameraProp(prop))) {
        VCamDiagLog(L"CtlProxy.Set %hs:%d -> E_PROP_ID_UNSUPPORTED (bad id)",
                    DomainName(isProcAmp), prop);
        return E_PROP_ID_UNSUPPORTED;
    }
    char req[192];
    snprintf(req, sizeof(req),
             "{\"op\":\"set\",\"domain\":\"%s\",\"id\":%d,\"value\":%d,"
             "\"flags\":%d}",
             DomainName(isProcAmp), prop, val, flags);
    std::string resp;
    std::wstring err;
    if (!ProxyTransact(req, resp, err)) {
        VCamDiagLog(L"CtlProxy.Set %hs:%d=%d -> E_PROP_ID_UNSUPPORTED (%s)",
                    DomainName(isProcAmp), prop, val, err.c_str());
        return E_PROP_ID_UNSUPPORTED;
    }
    bool ok = false;
    long hr = E_FAIL;
    if (!ParseOkHr(resp, ok, hr) || !ok) {
        VCamDiagLog(L"CtlProxy.Set %hs:%d=%d -> E_PROP_ID_UNSUPPORTED (hr=%d)",
                    DomainName(isProcAmp), prop, val, hr);
        return E_PROP_ID_UNSUPPORTED;
    }
    VCamDiagLog(L"CtlProxy.Set %hs:%d=%d flags=%d -> ok",
                DomainName(isProcAmp), prop, val, flags);
    return S_OK;
}

// --- COM-обёртки (делегирование времени жизни на внешника) ---

CProcAmpProxy::CProcAmpProxy() = default;
void CProcAmpProxy::Init(IUnknown* outer)
{
    m_outer = outer;
}
STDMETHODIMP CProcAmpProxy::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (!m_outer)
        return E_NOINTERFACE;
    if (riid == IID_IAMVideoProcAmp) {
        *ppv = static_cast<IAMVideoProcAmp*>(this);
        AddRef();
        return S_OK;
    }
    return m_outer->QueryInterface(riid, ppv); // IUnknown и всё остальное
}
STDMETHODIMP_(ULONG) CProcAmpProxy::AddRef()
{
    return m_outer ? m_outer->AddRef() : 0;
}
STDMETHODIMP_(ULONG) CProcAmpProxy::Release()
{
    return m_outer ? m_outer->Release() : 0;
}
STDMETHODIMP CProcAmpProxy::GetRange(long Property, long* pMin, long* pMax,
                                     long* pSteppingDelta, long* pDefault,
                                     long* pCapsFlags)
{
    return VCamProxyGetRange(true, Property, pMin, pMax, pSteppingDelta,
                             pDefault, pCapsFlags);
}
STDMETHODIMP CProcAmpProxy::Set(long Property, long lValue, long Flags)
{
    return VCamProxySet(true, Property, lValue, Flags);
}
STDMETHODIMP CProcAmpProxy::Get(long Property, long* lValue, long* Flags)
{
    return VCamProxyGet(true, Property, lValue, Flags);
}

CCameraProxy::CCameraProxy() = default;
void CCameraProxy::Init(IUnknown* outer)
{
    m_outer = outer;
}
STDMETHODIMP CCameraProxy::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv)
        return E_POINTER;
    *ppv = nullptr;
    if (!m_outer)
        return E_NOINTERFACE;
    if (riid == IID_IAMCameraControl) {
        *ppv = static_cast<IAMCameraControl*>(this);
        AddRef();
        return S_OK;
    }
    return m_outer->QueryInterface(riid, ppv);
}
STDMETHODIMP_(ULONG) CCameraProxy::AddRef()
{
    return m_outer ? m_outer->AddRef() : 0;
}
STDMETHODIMP_(ULONG) CCameraProxy::Release()
{
    return m_outer ? m_outer->Release() : 0;
}
STDMETHODIMP CCameraProxy::GetRange(long Property, long* pMin, long* pMax,
                                    long* pSteppingDelta, long* pDefault,
                                    long* pCapsFlags)
{
    return VCamProxyGetRange(false, Property, pMin, pMax, pSteppingDelta,
                             pDefault, pCapsFlags);
}
STDMETHODIMP CCameraProxy::Set(long Property, long lValue, long Flags)
{
    return VCamProxySet(false, Property, lValue, Flags);
}
STDMETHODIMP CCameraProxy::Get(long Property, long* lValue, long* Flags)
{
    return VCamProxyGet(false, Property, lValue, Flags);
}
