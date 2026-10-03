#pragma once

#include <windows.h>

#include <atlbase.h>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct IMFMediaSource;
struct IAMVideoProcAmp;
struct IAMCameraControl;

// Домен свойства (какому IAM-интерфейсу принадлежит).
enum class CameraControlDomain : int {
    ProcAmp = 0, // IAMVideoProcAmp (яркость, контраст, ...)
    Camera = 1,  // IAMCameraControl (зум, экспозиция, фокус, ...)
};

// Один элемент интроспекции: диапазон GetRange + текущее Get.
// supported=false = GetRange не удался (драйвер свойства не даёт);
// остальные поля тогда нули и клиенту такое свойство недоступно.
struct CameraControlDesc {
    CameraControlDomain domain = CameraControlDomain::ProcAmp;
    long id = 0; // VideoProcAmpProperty / CameraControlProperty
    std::wstring name; // L"brightness", L"focus", ...
    long minValue = 0;
    long maxValue = 0;
    long step = 0;
    long defaultValue = 0;
    long capsFlags = 0; // VideoProcAmpFlags_* / CameraControlFlags_* (что умеет)
    long curValue = 0;
    long curFlags = 0; // текущий режим (Auto=0x1 / Manual=0x2)
    HRESULT getHr = E_FAIL; // итог Get (S_OK = curValue/curFlags достоверны)
    bool supported = false;
};

// Фиксированные списки свойств (см. CameraControls.cpp):
// ProcAmp (9): brightness contrast hue saturation sharpness gamma whitebalance
//   backlightcompensation gain (без ColorEnable).
// Camera (7): pan tilt roll zoom exposure iris focus.
int CameraControlPropertyCount();
const CameraControlDesc& CameraControlPropertyTable(int index); // index в [0, count)

// Разбор "domain": L"procamp"|L"camera" (case-insensitive).
bool ParseCameraControlDomain(const std::wstring& s, CameraControlDomain& out);
// Разбор "id": имя из таблицы (case-insensitive) или число (валидируется по
// домену: procamp 0..9, camera 0..6). Возвращает numeric id + canonical name.
bool ParseCameraControlId(CameraControlDomain domain, const std::wstring& s,
                          long& outId, std::wstring& outName);
// Разбор "flags": L"auto"|L"manual" (case-insensitive) или число.
bool ParseCameraControlFlags(CameraControlDomain domain, const std::wstring& s,
                             long& outFlags);

// Интроспекция + применение контролов физической камеры (Brio 90, UVC).
// Два режима владения устройством:
//  - Open(id, name): standalone — OpenCameraActivate по матчингу
//    id→имя→подстрока, затем ActivateObject(IID_IMFMediaSource) + QI IAM*.
//    Стриминг НЕ запускается (SourceReader не создаётся) — устройство только
//    для control QI. Закрытие — Close (парный Shutdown/MFShutdown).
//  - Attach(mediaSrc): QI IAM* с уже открытого чужого IMFMediaSource
//    (использует CameraSource: второй ActivateObject не нужен, эксклюзивных
//    конфликтов нет). Владение источником остаётся у вызывающего; Detach
//    отпускает только IAM-указатели.
// Без открытого устройства все методы возвращают "пусто" (List — пустой вектор,
// Get/Set — S_FALSE/S_OK с hr E_HANDLE), падений нет. Потокобезопасно.
class CameraControls {
public:
    CameraControls();
    ~CameraControls();

    CameraControls(const CameraControls&) = delete;
    CameraControls& operator=(const CameraControls&) = delete;

    bool Open(const std::wstring& id, const std::wstring& name, std::wstring& err);
    void Attach(IMFMediaSource* mediaSrc);
    void Detach();
    void Close();
    bool IsOpen() const;

    // Интроспекция: для каждого свойства таблицы — GetRange + Get.
    // Пусто, если устройство не открыто. Отсутствие IAM-интерфейса целиком —
    // пустой дескриптор (не ошибка): свойства его домена unsupported.
    std::vector<CameraControlDesc> List() const;

    // Чтение одного свойства. Возвращает S_OK + desc; устройство закрыто или
    // свойство неизвестно → false (desc не валиден).
    bool Get(CameraControlDomain domain, long propId, CameraControlDesc& desc) const;

    // Применение: Set(domain, id, value, flags). Ручной Set поверх auto-режима:
    // сначала сбрасываем флаг в Manual, иначе драйвер игнорирует значение.
    // Возвращает hr операции + применённое значение (перечитанное Get).
    // Устройство закрыто / свойство неизвестно → E_HANDLE / E_INVALIDARG.
    HRESULT Set(CameraControlDomain domain, long propId, long value, long flags,
                long& appliedValue, long& appliedFlags);

private:
    HRESULT SetLocked(CameraControlDomain domain, long propId, long value, long flags,
                      long& appliedValue, long& appliedFlags);
    bool FillOneLocked(CameraControlDomain domain, long propId,
                       CameraControlDesc& desc) const;

    mutable std::mutex mutex_;
    ATL::CComPtr<IMFMediaSource> mediaSrc_; // только режим Open (для Shutdown)
    ATL::CComPtr<IAMVideoProcAmp> procAmp_;
    ATL::CComPtr<IAMCameraControl> camCtl_;
    bool mfUp_ = false;
    bool comUp_ = false;
};
