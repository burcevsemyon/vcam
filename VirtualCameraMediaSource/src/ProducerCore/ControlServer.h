#pragma once

#include <windows.h>

#include <string>
#include <vector>

class CameraControls;

// Управляющий канал продьюсера: именованный pipe `\\.\pipe\VCamControl.v1`.
// DACL — тот же SDDL, что у shared memory секции (см. SharedMemoryContract).
// JSON UTF-8, один запрос — одна строка с `\n`, один ответ — одна строка.
//
// Запросы:
//   {"op":"list"}
//   {"op":"get","domain":"procamp|camera","id":"brightness|0..9|..."}
//     id = имя свойства (case-insensitive) или число.
//   {"op":"set","domain":"...","id":"...","value":123}
//   {"op":"set","domain":"...","id":"...","value":123,"flags":"auto|manual|N"}
//     flags по умолчанию manual (ручной Set поверх auto сбрасывает режим
//     в Manual — см. CameraControls::Set).
//
// Ответы (hr — signed decimal HRESULT):
//   list (устройство открыто):
//     {"ok":true,"hr":0,"controls":[
//       {"domain":"procamp","id":0,"name":"brightness","min":0,"max":255,
//        "step":1,"def":128,"caps":3,"cur":128,"flags":2,"supported":true},
//       ... все 16 свойств ...]}
//   list (устройства нет): {"ok":true,"hr":0,"controls":[]}
//   get/set ok:  {"ok":true,"hr":0,"cur":128,"flags":2}
//   ошибка:      {"ok":false,"hr":-2147024891,"error":"..."} (текст ASCII)
//
// Сервер живёт там, где открыто устройство: владелец — CameraSource
// (Attach к своему IMFMediaSource; старт сервера в Open, стоп в Shutdown).
// Второй сервер в системе запрещён: Start возвращает
// HRESULT_FROM_WIN32(ERROR_PIPE_BUSY). Ошибка старта сервера НЕ роняет
// источник (стриминг продолжается без управления).
class ControlServer {
public:
    ControlServer();
    ~ControlServer();

    ControlServer(const ControlServer&) = delete;
    ControlServer& operator=(const ControlServer&) = delete;

    // controls — не владеем (живёт дольше сервера: член CameraSource).
    // S_OK = запущен; S_FALSE = уже запущен этот экземпляр;
    // ERROR_PIPE_BUSY = сервер уже есть в системе; прочее = pipe/DACL ошибка.
    HRESULT Start(CameraControls* controls);
    void Stop();
    bool IsRunning() const;

    static constexpr const wchar_t* kPipeName = L"\\\\.\\pipe\\VCamControl.v1";
    static constexpr const wchar_t* kMutexName = L"VCamControlServer.Instance";

private:
    static DWORD WINAPI AcceptProc(LPVOID self);
    static DWORD WINAPI ClientProc(LPVOID param);
    void AcceptLoop();
    void HandleClient(HANDLE pipe);
    std::string ProcessLine(const std::string& line);

    CameraControls* controls_ = nullptr; // не владеем
    HANDLE mutex_ = nullptr;
    HANDLE stopEvent_ = nullptr;
    HANDLE acceptThread_ = nullptr;
    mutable CRITICAL_SECTION cs_;
    bool running_ = false;
    bool stopping_ = false;
    struct Client {
        HANDLE thread = nullptr;
        HANDLE pipe = nullptr;
    };
    std::vector<Client> clients_;
};

// Синхронный клиент (для harness-теста и Sub 2 — виртуалки):
// подключается к pipe, шлёт одну JSON-строку, ждёт одну строку ответа.
// requestJson/responseJson — UTF-8 без `\n` (фрейминг добавляется внутри).
bool ControlClientRequest(const std::string& requestJson, std::string& responseJson,
                          DWORD timeoutMs, std::wstring& err);
