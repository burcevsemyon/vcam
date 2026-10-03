#pragma once

// Тонкий прокси IAMVideoProcAmp / IAMCameraControl поверх управляющего
// pipe-канала продьюсера (половина Sub 2 задачи vcam-camera-proxy).
//
// Протокол — см. src/ProducerCore/ControlServer.h (Sub 1): pipe
// `\\.\pipe\VCamControl.v1`, JSON UTF-8 построчно, опсы list/get/set.
// MediaSource НЕ линкуется с ProducerCore (проверено по MediaSource.vcxproj —
// ProjectReference нет), поэтому транспорт здесь продублирован минимально
// (та же семантика, что ControlClientRequest: ретраи NOT_FOUND до дедлайна,
// BUSY через WaitNamedPipe), а не переиспользован. Ответы парсятся своим
// мини-DOM (серверные ответы — машинные, ASCII, плоские объекты + один
// массив controls[]).
//
// Семантика ошибок (по спеке Sub 2):
//   - каждый вызов Get/Range/Set — синхронный запрос, таймаут kProxyTimeoutMs;
//   - сервера нет / свойство не поддержано / ответ не распарсился /
//     неизвестный id → E_PROP_ID_UNSUPPORTED (0x80070490, «ручек нет»),
//     а НЕ E_FAIL — так отличаем отсутствие ручки от ошибки;
//   - нулевые out-указатели → E_POINTER (до pipe).
// Локальная валидация id — как у Sub 1: procamp 0..9 кроме 6 (ColorEnable
// вне спеки), camera 0..6. Остальное решает сервер, любое !ok маппится
// в E_PROP_ID_UNSUPPORTED.
// Виртуалка про режим (static/video/camera) не знает — всегда проксирует.
// Потокобезопасность: состояния нет (все переменные локальные), Init
// вызывается один раз в FinalConstruct до публикации объекта (MTA-safe).

#include <windows.h>
#include <strmif.h> // IAMVideoProcAmp / IAMCameraControl

// Перечисленные домены для VCamProxy* ниже.
bool VCamProxyIsProcAmpProp(long prop); // 0..5,7..9
bool VCamProxyIsCameraProp(long prop);  // 0..6

// Синхронные запросы к pipe-серверу. Успех = S_OK + заполненные out.
// Отсутствие сервера/поддержки/битый ответ = E_PROP_ID_UNSUPPORTED.
HRESULT VCamProxyGetRange(bool isProcAmp, long prop, long* pMin, long* pMax,
                          long* pStep, long* pDef, long* pCaps);
HRESULT VCamProxyGet(bool isProcAmp, long prop, long* pVal, long* pFlags);
HRESULT VCamProxySet(bool isProcAmp, long prop, long val, long flags);

// COM-обёртки для QI CMediaSource: время жизни — внешний источник
// (AddRef/Release/QI-делегирование на m_outer, своих ссылок не держим).
// m_outer — канонический IUnknown внешника; задаётся в Init до публикации.
class CProcAmpProxy : public IAMVideoProcAmp {
public:
    CProcAmpProxy();
    void Init(IUnknown* outer); // outer без AddRef (внешник владеет нами)
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP GetRange(long Property, long* pMin, long* pMax,
                          long* pSteppingDelta, long* pDefault,
                          long* pCapsFlags) override;
    STDMETHODIMP Set(long Property, long lValue, long Flags) override;
    STDMETHODIMP Get(long Property, long* lValue, long* Flags) override;

private:
    IUnknown* m_outer = nullptr; // weak, живёт дольше нас (член внешника)
};

class CCameraProxy : public IAMCameraControl {
public:
    CCameraProxy();
    void Init(IUnknown* outer);
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override;
    STDMETHODIMP_(ULONG) Release() override;
    STDMETHODIMP GetRange(long Property, long* pMin, long* pMax,
                          long* pSteppingDelta, long* pDefault,
                          long* pCapsFlags) override;
    STDMETHODIMP Set(long Property, long lValue, long Flags) override;
    STDMETHODIMP Get(long Property, long* lValue, long* Flags) override;

private:
    IUnknown* m_outer = nullptr;
};
