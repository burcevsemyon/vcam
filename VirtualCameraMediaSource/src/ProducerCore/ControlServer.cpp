// Pipe-сервер управляющего канала (см. ControlServer.h — протокол).
// Фрейминг: UTF-8 построчно (`\n`). Сервер: overlapped-accept + поток на
// клиента. Остановка: stopEvent будит accept, DisconnectNamedPipe рвёт
// висящие ReadFile, потоки join'ятся в Stop.

#include "ControlServer.h"

#include <atlbase.h>
#include <sddl.h>

#include <cstdio>
#include <cwctype>
#include <utility>

#include "CameraControls.h"
#include "SharedMemoryContract.h"
#include "WinUtil.h"

#pragma comment(lib, "advapi32.lib")

namespace {

constexpr DWORD kPipeTimeoutMs = 5000;
constexpr size_t kMaxLine = 256 * 1024;

using vcam::WinErr;

// --- UTF-8 <-> wide (только для id/domain/flags из запроса) ---

bool Utf8ToWide(const std::string& s, std::wstring& out)
{
    if (s.empty()) { out.clear(); return true; }
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                (int)s.size(), nullptr, 0);
    if (n <= 0) return false;
    out.resize((size_t)n);
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                               (int)s.size(), &out[0], n) == n;
}

// --- Мини-JSON: разбор объекта с плоскими значениями ---

struct JsonValue {
    bool isString = false;
    bool isNumber = false;
    bool isBool = false;
    std::string str; // decoded (escapes) для строк
    long num = 0;
    bool boolean = false;
};

void SkipWs(const char* s, size_t n, size_t& p)
{
    while (p < n && (s[p] == ' ' || s[p] == '\t' || s[p] == '\r' || s[p] == '\n'))
        p++;
}

// Парсит JSON-строку с позиции p (s[p]=='"'), декодирует escapes. -1 = ошибка.
bool ParseJsonString(const char* s, size_t n, size_t& p, std::string& out)
{
    out.clear();
    if (p >= n || s[p] != '"') return false;
    p++;
    while (p < n) {
        char c = s[p];
        if (c == '"') { p++; return true; }
        if (c == '\\') {
            p++;
            if (p >= n) return false;
            char e = s[p];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                // \uXXXX -> UTF-8 (BMP; суррогаты склеиваем).
                if (p + 4 >= n) return false;
                auto hex = [&](size_t k) -> int {
                    char h = s[k];
                    if (h >= '0' && h <= '9') return h - '0';
                    if (h >= 'a' && h <= 'f') return h - 'a' + 10;
                    if (h >= 'A' && h <= 'F') return h - 'A' + 10;
                    return -1;
                };
                unsigned cp = 0;
                for (int k = 1; k <= 4; k++) {
                    int h = hex(p + (size_t)k);
                    if (h < 0) return false;
                    cp = cp * 16 + (unsigned)h;
                }
                p += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && p + 5 < n && s[p + 1] == '\\' &&
                    s[p + 2] == 'u') {
                    unsigned lo = 0;
                    bool ok = true;
                    for (int k = 3; k <= 6; k++) {
                        int h = hex(p + (size_t)k);
                        if (h < 0) { ok = false; break; }
                        lo = lo * 16 + (unsigned)h;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        p += 6;
                    }
                }
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) {
                    out += (char)(0xC0 | (cp >> 6));
                    out += (char)(0x80 | (cp & 0x3F));
                } else if (cp < 0x10000) {
                    out += (char)(0xE0 | (cp >> 12));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                } else {
                    out += (char)(0xF0 | (cp >> 18));
                    out += (char)(0x80 | ((cp >> 12) & 0x3F));
                    out += (char)(0x80 | ((cp >> 6) & 0x3F));
                    out += (char)(0x80 | (cp & 0x3F));
                }
                break;
            }
            default: return false;
            }
            p++;
            continue;
        }
        if ((unsigned char)c < 0x20) return false;
        out += c;
        p++;
    }
    return false;
}

bool ParseJsonNumber(const char* s, size_t n, size_t& p, long& out)
{
    size_t start = p;
    if (p < n && (s[p] == '-' || s[p] == '+')) p++;
    size_t digits = p;
    while (p < n && s[p] >= '0' && s[p] <= '9') p++;
    if (p == digits) return false;
    // Дробь/экспонента для протокола не нужны — отбрасываем как ошибку.
    if (p < n && (s[p] == '.' || s[p] == 'e' || s[p] == 'E')) return false;
    out = strtol(std::string(s + start, p - start).c_str(), nullptr, 10);
    return true;
}

// Плоский объект {"k":v,...}: массивы/вложенные объекты не поддерживаем.
bool ParseJsonObject(const std::string& line,
                     std::vector<std::pair<std::string, JsonValue>>& fields)
{
    fields.clear();
    const char* s = line.c_str();
    size_t n = line.size();
    size_t p = 0;
    SkipWs(s, n, p);
    if (p >= n || s[p] != '{') return false;
    p++;
    SkipWs(s, n, p);
    if (p < n && s[p] == '}') { p++; SkipWs(s, n, p); return p == n; }
    for (;;) {
        SkipWs(s, n, p);
        std::string key;
        if (!ParseJsonString(s, n, p, key)) return false;
        SkipWs(s, n, p);
        if (p >= n || s[p] != ':') return false;
        p++;
        SkipWs(s, n, p);
        JsonValue v;
        if (p < n && s[p] == '"') {
            if (!ParseJsonString(s, n, p, v.str)) return false;
            v.isString = true;
        } else if (p + 4 <= n && memcmp(s + p, "true", 4) == 0) {
            v.isBool = true; v.boolean = true; p += 4;
        } else if (p + 5 <= n && memcmp(s + p, "false", 5) == 0) {
            v.isBool = true; v.boolean = false; p += 5;
        } else if (!ParseJsonNumber(s, n, p, v.num)) {
            return false;
        } else {
            v.isNumber = true;
        }
        fields.emplace_back(key, v);
        SkipWs(s, n, p);
        if (p >= n) return false;
        if (s[p] == ',') { p++; continue; }
        if (s[p] == '}') { p++; SkipWs(s, n, p); return p == n; }
        return false;
    }
}

const JsonValue* FindField(
    const std::vector<std::pair<std::string, JsonValue>>& fields, const char* key)
{
    for (const auto& f : fields)
        if (f.first == key) return &f.second;
    return nullptr;
}

// --- Построение ответов (имена/домены — ASCII из фиксированной таблицы) ---

void AppendEscaped(std::string& out, const char* s)
{
    for (const char* p = s; *p; p++) {
        switch (*p) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default: out += *p; break;
        }
    }
}

const char* DomainName(CameraControlDomain d)
{
    return d == CameraControlDomain::ProcAmp ? "procamp" : "camera";
}

std::string WideToUtf8(const std::wstring& w)
{
    if (w.empty()) return std::string();
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(),
                                nullptr, 0, nullptr, nullptr);
    if (n <= 0) return std::string();
    std::string out((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), &out[0], n,
                        nullptr, nullptr);
    return out;
}

std::string ErrorResponse(HRESULT hr, const char* msg)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"hr\":%d,\"error\":\"",
             (int)hr);
    std::string out(buf);
    AppendEscaped(out, msg);
    out += "\"}";
    return out;
}

} // namespace

ControlServer::ControlServer()
{
    InitializeCriticalSection(&cs_);
}

ControlServer::~ControlServer()
{
    Stop();
    DeleteCriticalSection(&cs_);
}

HRESULT ControlServer::Start(CameraControls* controls)
{
    if (!controls) return E_INVALIDARG;
    EnterCriticalSection(&cs_);
    if (running_) {
        LeaveCriticalSection(&cs_);
        return S_FALSE;
    }
    // Единственный сервер в системе (иначе два держателя камеры делили бы
    // один pipe — клиенты ходили бы к случайному).
    // Локалы в CHandle: ранние return не текут, в члены — Detach при успехе.
    ATL::CHandle m(CreateMutexW(nullptr, FALSE, kMutexName));
    if (!m) {
        DWORD e = GetLastError();
        LeaveCriticalSection(&cs_);
        return HRESULT_FROM_WIN32(e);
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        LeaveCriticalSection(&cs_);
        return HRESULT_FROM_WIN32(ERROR_PIPE_BUSY);
    }
    ATL::CHandle stop(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!stop) {
        DWORD e = GetLastError();
        LeaveCriticalSection(&cs_);
        return HRESULT_FROM_WIN32(e);
    }
    controls_ = controls;
    mutex_ = m.Detach();
    stopEvent_ = stop.Detach();
    stopping_ = false;
    ATL::CHandle t(CreateThread(nullptr, 0, AcceptProc, this, 0, nullptr));
    if (!t) {
        DWORD e = GetLastError();
        CloseHandle(stopEvent_);
        CloseHandle(mutex_);
        stopEvent_ = nullptr;
        mutex_ = nullptr;
        controls_ = nullptr;
        LeaveCriticalSection(&cs_);
        return HRESULT_FROM_WIN32(e);
    }
    acceptThread_ = t.Detach();
    running_ = true;
    LeaveCriticalSection(&cs_);
    return S_OK;
}

void ControlServer::Stop()
{
    HANDLE accept = nullptr;
    EnterCriticalSection(&cs_);
    if (!running_) {
        LeaveCriticalSection(&cs_);
        return;
    }
    stopping_ = true;
    if (stopEvent_) SetEvent(stopEvent_);
    accept = acceptThread_;
    acceptThread_ = nullptr;
    // Рвём висящие ReadFile клиентов, чтобы их потоки вышли и join не висел.
    for (const Client& c : clients_) {
        if (c.pipe) DisconnectNamedPipe(c.pipe);
    }
    LeaveCriticalSection(&cs_);

    if (accept) {
        WaitForSingleObject(accept, INFINITE);
        CloseHandle(accept);
    }
    EnterCriticalSection(&cs_);
    for (const Client& c : clients_) {
        if (c.thread) {
            WaitForSingleObject(c.thread, INFINITE);
            CloseHandle(c.thread);
        }
        // pipe-хэндл закрывает сам поток клиента (ClientProc).
    }
    clients_.clear();
    if (stopEvent_) { CloseHandle(stopEvent_); stopEvent_ = nullptr; }
    if (mutex_) { CloseHandle(mutex_); mutex_ = nullptr; }
    controls_ = nullptr;
    running_ = false;
    stopping_ = false;
    LeaveCriticalSection(&cs_);
}

bool ControlServer::IsRunning() const
{
    EnterCriticalSection(const_cast<LPCRITICAL_SECTION>(&cs_));
    bool r = running_;
    LeaveCriticalSection(const_cast<LPCRITICAL_SECTION>(&cs_));
    return r;
}

DWORD WINAPI ControlServer::AcceptProc(LPVOID self)
{
    static_cast<ControlServer*>(self)->AcceptLoop();
    return 0;
}

struct ClientParam {
    ControlServer* server;
    HANDLE pipe;
};

DWORD WINAPI ControlServer::ClientProc(LPVOID param)
{
    std::unique_ptr<ClientParam> cp(static_cast<ClientParam*>(param));
    cp->server->HandleClient(cp->pipe);
    return 0;
}

void ControlServer::AcceptLoop()
{
    for (;;) {
        // DACL как у shm-секции: все локальные могут подключаться.
        PSECURITY_DESCRIPTOR psd = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                vcam::VCamDacSddl, SDDL_REVISION_1, &psd, nullptr)) {
            psd = nullptr; // без DACL — только fallback, pipe всё равно нужен
        }
        SECURITY_ATTRIBUTES sa = {};
        sa.nLength = sizeof(sa);
        sa.lpSecurityDescriptor = psd;
        sa.bInheritHandle = FALSE;

        ATL::CHandle pipe(CreateNamedPipeW(
            kPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 8192, 8192, kPipeTimeoutMs, &sa));
        if (psd) LocalFree(psd);
        if (pipe == INVALID_HANDLE_VALUE) {
            DWORD e = GetLastError();
            OutputDebugStringW(
                (L"[ControlServer] CreateNamedPipe failed: " + WinErr(e) + L"\n")
                    .c_str());
            Sleep(500);
            EnterCriticalSection(&cs_);
            bool stop = stopping_;
            LeaveCriticalSection(&cs_);
            if (stop) break;
            continue;
        }

        OVERLAPPED ov = {};
        ATL::CHandle connEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        ov.hEvent = connEvent;
        BOOL cc = ConnectNamedPipe(pipe, &ov);
        DWORD e = GetLastError();
        if (cc) e = ERROR_PIPE_CONNECTED; // редкий sync-успех
        if (!cc && e == ERROR_IO_PENDING) {
            HANDLE w[2] = { stopEvent_, static_cast<HANDLE>(connEvent) };
            DWORD r = WaitForMultipleObjects(2, w, FALSE, INFINITE);
            if (r == WAIT_OBJECT_0) {
                CancelIo(pipe); // будим ConnectNamedPipe
                break;
            }
            e = ERROR_PIPE_CONNECTED;
        } else if (!cc && e != ERROR_PIPE_CONNECTED) {
            // Клиент подключился и сразу отвалился между Create и Connect.
            EnterCriticalSection(&cs_);
            bool stop = stopping_;
            LeaveCriticalSection(&cs_);
            if (stop) break;
            continue;
        }

        EnterCriticalSection(&cs_);
        bool stop = stopping_;
        LeaveCriticalSection(&cs_);
        if (stop) {
            DisconnectNamedPipe(pipe);
            break;
        }

        HANDLE rawPipe = pipe.Detach();
        auto* cp = new (std::nothrow) ClientParam{ this, rawPipe };
        if (!cp) {
            DisconnectNamedPipe(rawPipe);
            CloseHandle(rawPipe);
            continue;
        }
        ATL::CHandle t(CreateThread(nullptr, 0, ClientProc, cp, 0, nullptr));
        if (!t) {
            delete cp;
            DisconnectNamedPipe(rawPipe);
            CloseHandle(rawPipe);
            continue;
        }
        EnterCriticalSection(&cs_);
        clients_.push_back({ t.Detach(), rawPipe });
        LeaveCriticalSection(&cs_);
    }
}

void ControlServer::HandleClient(HANDLE pipe)
{
    std::string pending;
    char chunk[4096];
    for (;;) {
        DWORD read = 0;
        BOOL ok = ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr);
        if (!ok || read == 0) break; // EOF/разрыв (в т.ч. Disconnect в Stop)
        pending.append(chunk, read);
        if (pending.size() > kMaxLine + 1024) break; // мусор без \n — рвём
        size_t pos = 0;
        while ((pos = pending.find('\n')) != std::string::npos) {
            std::string line = pending.substr(0, pos);
            pending.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            std::string resp = ProcessLine(line);
            resp += '\n';
            DWORD written = 0;
            if (!WriteFile(pipe, resp.data(), (DWORD)resp.size(), &written,
                           nullptr) ||
                written != resp.size()) {
                pending.clear();
                break;
            }
        }
        if (pending.empty() == false && pending.size() > kMaxLine) break;
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    // Убираем себя из учёта (Stop без нас закроет thread-хэндл; ждать себя нельзя).
    ATL::CHandle self;
    {
        HANDLE rawMe = nullptr;
        BOOL dup = DuplicateHandle(GetCurrentProcess(), GetCurrentThread(),
                                   GetCurrentProcess(), &rawMe, 0, FALSE,
                                   DUPLICATE_SAME_ACCESS);
        ATL::CHandle me(rawMe);
        EnterCriticalSection(&cs_);
        for (auto it = clients_.begin(); it != clients_.end(); ++it) {
            if (dup && static_cast<HANDLE>(me) != nullptr && it->thread) {
                DWORD idIt = GetThreadId(it->thread);
                DWORD idMe = GetCurrentThreadId();
                if (idIt == idMe) {
                    self.Attach(it->thread); // закроем сами
                    it->thread = nullptr;
                    it->pipe = nullptr;
                    clients_.erase(it);
                    break;
                }
            }
        }
        LeaveCriticalSection(&cs_);
    }
}

std::string ControlServer::ProcessLine(const std::string& line)
{
    std::vector<std::pair<std::string, JsonValue>> f;
    if (!ParseJsonObject(line, f)) return ErrorResponse(E_INVALIDARG, "bad json");

    const JsonValue* op = FindField(f, "op");
    if (!op || !op->isString) return ErrorResponse(E_INVALIDARG, "missing op");
    EnterCriticalSection(&cs_);
    CameraControls* ctl = controls_;
    LeaveCriticalSection(&cs_);

    if (op->str == "list") {
        std::string out("{\"ok\":true,\"hr\":0,\"controls\":[");
        if (ctl) {
            std::vector<CameraControlDesc> v = ctl->List();
            bool first = true;
            for (const CameraControlDesc& d : v) {
                if (!first) out += ',';
                first = false;
                char buf[256];
                snprintf(buf, sizeof(buf),
                         "{\"domain\":\"%s\",\"id\":%ld,\"name\":\"%s\","
                         "\"min\":%ld,\"max\":%ld,\"step\":%ld,\"def\":%ld,"
                         "\"caps\":%ld,\"cur\":%ld,\"flags\":%ld,"
                         "\"supported\":%s}",
                         DomainName(d.domain), d.id, WideToUtf8(d.name).c_str(),
                         d.minValue, d.maxValue, d.step, d.defaultValue,
                         d.capsFlags, d.curValue, d.curFlags,
                         d.supported ? "true" : "false");
                out += buf;
            }
        }
        out += "]}";
        return out;
    }

    if (op->str == "get" || op->str == "set") {
        if (!ctl) return ErrorResponse(HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED),
                                       "no device");
        const JsonValue* dj = FindField(f, "domain");
        const JsonValue* ij = FindField(f, "id");
        if (!dj || !dj->isString) return ErrorResponse(E_INVALIDARG, "missing domain");
        std::wstring wdom;
        if (!Utf8ToWide(dj->str, wdom)) return ErrorResponse(E_INVALIDARG, "bad domain");
        CameraControlDomain dom;
        if (!ParseCameraControlDomain(wdom, dom))
            return ErrorResponse(E_INVALIDARG, "unknown domain");
        long propId = 0;
        if (!ij) return ErrorResponse(E_INVALIDARG, "missing id");
        if (ij->isNumber) {
            // Числовой id валидируем через таблицу (дыра procamp:6 отсекается).
            std::wstring nm;
            wchar_t tmp[32];
            swprintf_s(tmp, L"%ld", ij->num);
            if (!ParseCameraControlId(dom, tmp, propId, nm))
                return ErrorResponse(E_INVALIDARG, "unknown id");
        } else if (ij->isString) {
            std::wstring wid;
            if (!Utf8ToWide(ij->str, wid))
                return ErrorResponse(E_INVALIDARG, "bad id");
            std::wstring nm;
            if (!ParseCameraControlId(dom, wid, propId, nm))
                return ErrorResponse(E_INVALIDARG, "unknown id");
        } else {
            return ErrorResponse(E_INVALIDARG, "bad id");
        }

        if (op->str == "get") {
            CameraControlDesc d;
            if (!ctl->Get(dom, propId, d) || d.name.empty())
                return ErrorResponse(E_INVALIDARG, "unknown id");
            if (!d.supported)
                return ErrorResponse(HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND),
                                     "not supported");
            char buf[128];
            snprintf(buf, sizeof(buf), "{\"ok\":true,\"hr\":0,\"cur\":%ld,\"flags\":%ld}",
                     d.curValue, d.curFlags);
            return std::string(buf);
        }

        // set
        const JsonValue* vj = FindField(f, "value");
        if (!vj || !vj->isNumber)
            return ErrorResponse(E_INVALIDARG, "missing value");
        long flags = 0x2; // manual по умолчанию
        const JsonValue* fj = FindField(f, "flags");
        if (fj) {
            if (fj->isNumber) {
                flags = fj->num;
            } else if (fj->isString) {
                std::wstring wf;
                if (!Utf8ToWide(fj->str, wf))
                    return ErrorResponse(E_INVALIDARG, "bad flags");
                if (!ParseCameraControlFlags(dom, wf, flags))
                    return ErrorResponse(E_INVALIDARG, "unknown flags");
            } else {
                return ErrorResponse(E_INVALIDARG, "bad flags");
            }
        }
        long applied = 0, appliedFlags = 0;
        HRESULT hr = ctl->Set(dom, propId, vj->num, flags, applied, appliedFlags);
        if (FAILED(hr)) {
            if (hr == HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND))
                return ErrorResponse(hr, "not supported");
            if (hr == E_HANDLE) return ErrorResponse(hr, "no device");
            return ErrorResponse(hr, "set failed");
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "{\"ok\":true,\"hr\":0,\"cur\":%ld,\"flags\":%ld}",
                 applied, appliedFlags);
        return std::string(buf);
    }

    return ErrorResponse(E_INVALIDARG, "unknown op");
}

bool ControlClientRequest(const std::string& requestJson, std::string& responseJson,
                          DWORD timeoutMs, std::wstring& err)
{
    responseJson.clear();
    // Сервер мог только стартовать (accept-поток ещё не создал инстанс):
    // NOT_FOUND — ретраим до дедлайна, BUSY — ждём через WaitNamedPipe.
    ULONGLONG openDeadline = GetTickCount64() + (timeoutMs < 100 ? 100 : timeoutMs);
    HANDLE h = INVALID_HANDLE_VALUE;
    DWORD openErr = 0;
    for (;;) {
        h = CreateFileW(ControlServer::kPipeName, GENERIC_READ | GENERIC_WRITE,
                        0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        openErr = GetLastError();
        if (openErr == ERROR_PIPE_BUSY) {
            ULONGLONG now = GetTickCount64();
            DWORD left = (now >= openDeadline) ? 0 : (DWORD)(openDeadline - now);
            if (left == 0 || !WaitNamedPipeW(ControlServer::kPipeName, left)) {
                err = L"сервер управления недоступен (таймаут)";
                return false;
            }
            continue;
        }
        if (openErr != ERROR_FILE_NOT_FOUND) break;
        if (GetTickCount64() >= openDeadline) break;
        Sleep(20);
    }
    if (h == INVALID_HANDLE_VALUE) {
        err = L"сервер управления недоступен: " + WinErr(openErr);
        return false;
    }
    ATL::CHandle pipe(h);

    DWORD mode = PIPE_READMODE_BYTE;
    SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);

    std::string line = requestJson;
    line += '\n';
    DWORD written = 0;
    if (!WriteFile(pipe, line.data(), (DWORD)line.size(), &written, nullptr) ||
        written != line.size()) {
        err = L"запись в pipe: " + WinErr(GetLastError());
        return false;
    }

    std::string acc;
    char chunk[4096];
    ULONGLONG deadline = GetTickCount64() + timeoutMs;
    for (;;) {
        ULONGLONG now = GetTickCount64();
        if (now >= deadline) {
            err = L"таймаут ответа сервера управления";
            return false;
        }
        DWORD wait = (DWORD)(deadline - now);
        DWORD avail = 0;
        BOOL peek = PeekNamedPipe(pipe, nullptr, 0, nullptr, &avail, nullptr);
        if (!peek) {
            err = L"сервер закрыл соединение: " + WinErr(GetLastError());
            return false;
        }
        if (avail == 0) {
            Sleep(wait > 20 ? 20 : wait);
            continue;
        }
        DWORD read = 0;
        if (!ReadFile(pipe, chunk, sizeof(chunk), &read, nullptr) || read == 0) {
            err = L"чтение pipe: " + WinErr(GetLastError());
            return false;
        }
        acc.append(chunk, read);
        if (acc.size() > kMaxLine) {
            err = L"ответ сервера слишком длинный";
            return false;
        }
        size_t pos = acc.find('\n');
        if (pos != std::string::npos) {
            responseJson = acc.substr(0, pos);
            if (!responseJson.empty() && responseJson.back() == '\r')
                responseJson.pop_back();
            return true;
        }
    }
}
