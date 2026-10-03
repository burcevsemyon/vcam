// Интроспекция и применение контролов физической камеры через
// IAMVideoProcAmp / IAMCameraControl (DirectShow, strmif.h). Интерфейсы
// запрашиваются QI с IMFMediaSource камеры (UVC KS-прокси отдаёт их там).

#include "CameraControls.h"

#include <cwctype>
#include <utility>

#include <mfapi.h>
#include <mfidl.h>
#include <strmif.h>

#include "CameraDevices.h"

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mf.lib")
#pragma comment(lib, "strmiids.lib")

namespace {

// Флаги режимов едины по значению в обоих интерфейсах:
// VideoProcAmp_Flags_Auto/CameraControl_Flags_Auto = 0x1, Manual = 0x2.
constexpr long kFlagAuto = 0x1;
constexpr long kFlagManual = 0x2;

struct PropRow {
    CameraControlDomain domain;
    long id;
    const wchar_t* name;
};

#define ROW(d, id, nm) { CameraControlDomain::d, id, L##nm }

// Фиксированные списки спеки: ProcAmp без ColorEnable (6).
constexpr PropRow kProps[] = {
    ROW(ProcAmp, 0, "brightness"),            // VideoProcAmp_Brightness
    ROW(ProcAmp, 1, "contrast"),              // VideoProcAmp_Contrast
    ROW(ProcAmp, 2, "hue"),                   // VideoProcAmp_Hue
    ROW(ProcAmp, 3, "saturation"),            // VideoProcAmp_Saturation
    ROW(ProcAmp, 4, "sharpness"),             // VideoProcAmp_Sharpness
    ROW(ProcAmp, 5, "gamma"),                 // VideoProcAmp_Gamma
    ROW(ProcAmp, 7, "whitebalance"),          // VideoProcAmp_WhiteBalance
    ROW(ProcAmp, 8, "backlightcompensation"), // VideoProcAmp_BacklightCompensation
    ROW(ProcAmp, 9, "gain"),                  // VideoProcAmp_Gain
    ROW(Camera, 0, "pan"),                    // CameraControl_Pan
    ROW(Camera, 1, "tilt"),                   // CameraControl_Tilt
    ROW(Camera, 2, "roll"),                   // CameraControl_Roll
    ROW(Camera, 3, "zoom"),                   // CameraControl_Zoom
    ROW(Camera, 4, "exposure"),               // CameraControl_Exposure
    ROW(Camera, 5, "iris"),                   // CameraControl_Iris
    ROW(Camera, 6, "focus"),                  // CameraControl_Focus
};

constexpr int kPropCount = (int)(sizeof(kProps) / sizeof(kProps[0]));

bool EqualsNoCase(const std::wstring& a, const std::wstring& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (towlower(a[i]) != towlower(b[i])) return false;
    return true;
}

std::wstring HrHex(HRESULT hr)
{
    wchar_t buf[16];
    swprintf(buf, 16, L"0x%08X", (unsigned)hr);
    return std::wstring(buf);
}

} // namespace

int CameraControlPropertyCount()
{
    return kPropCount;
}

const CameraControlDesc& CameraControlPropertyTable(int index)
{
    static CameraControlDesc table[kPropCount];
    static bool init = false;
    if (!init) {
        for (int i = 0; i < kPropCount; i++) {
            table[i].domain = kProps[i].domain;
            table[i].id = kProps[i].id;
            table[i].name = kProps[i].name;
        }
        init = true;
    }
    return table[index];
}

bool ParseCameraControlDomain(const std::wstring& s, CameraControlDomain& out)
{
    if (EqualsNoCase(s, L"procamp") || EqualsNoCase(s, L"videoprocamp") ||
        EqualsNoCase(s, L"proc_amp")) {
        out = CameraControlDomain::ProcAmp;
        return true;
    }
    if (EqualsNoCase(s, L"camera") || EqualsNoCase(s, L"cameracontrol") ||
        EqualsNoCase(s, L"cam")) {
        out = CameraControlDomain::Camera;
        return true;
    }
    return false;
}

namespace {

const PropRow* FindRow(CameraControlDomain domain, const std::wstring& name)
{
    for (const PropRow& r : kProps)
        if (r.domain == domain && EqualsNoCase(r.name, name)) return &r;
    // Числовой id: валидируем по домену (procamp 0..9, camera 0..6) и ищем
    // строку в таблице (у procamp дыра на 6=ColorEnable — вне спеки).
    wchar_t* end = nullptr;
    long n = wcstol(name.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') return nullptr;
    if (domain == CameraControlDomain::ProcAmp && (n < 0 || n > 9)) return nullptr;
    if (domain == CameraControlDomain::Camera && (n < 0 || n > 6)) return nullptr;
    for (const PropRow& r : kProps)
        if (r.domain == domain && r.id == n) return &r;
    return nullptr; // procamp:6 (ColorEnable) — не из спеки
}

} // namespace

bool ParseCameraControlId(CameraControlDomain domain, const std::wstring& s,
                          long& outId, std::wstring& outName)
{
    const PropRow* r = FindRow(domain, s);
    if (!r) return false;
    outId = r->id;
    outName.assign(r->name);
    return true;
}

bool ParseCameraControlFlags(CameraControlDomain domain, const std::wstring& s,
                             long& outFlags)
{
    (void)domain; // значения Auto/Manual едины в обоих интерфейсах
    if (EqualsNoCase(s, L"auto")) { outFlags = kFlagAuto; return true; }
    if (EqualsNoCase(s, L"manual")) { outFlags = kFlagManual; return true; }
    wchar_t* end = nullptr;
    long n = wcstol(s.c_str(), &end, 10);
    if (end == nullptr || *end != L'\0') return false;
    outFlags = n;
    return true;
}

CameraControls::CameraControls() = default;

CameraControls::~CameraControls()
{
    Close();
}

bool CameraControls::Open(const std::wstring& id, const std::wstring& name,
                          std::wstring& err)
{
    Close();

    if (id.empty() && name.empty()) {
        err = L"камера не выбрана";
        return false;
    }

    CameraDeviceInfo devInfo;
    IMFActivate* rawAct = OpenCameraActivate(id, name, devInfo);
    ATL::CComPtr<IMFActivate> act;
    act.Attach(rawAct); // владение переходит нам (может быть nullptr)
    if (!act) {
        err = L"камера не найдена";
        return false;
    }

    HRESULT hrCo = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comHere = (hrCo == S_OK);
    HRESULT hr = MFStartup(MF_VERSION, MFSTARTUP_FULL);
    if (FAILED(hr)) {
        if (comHere) CoUninitialize();
        err = L"MFStartup failed: " + HrHex(hr);
        return false;
    }

    ATL::CComPtr<IMFMediaSource> msrc;
    hr = act->ActivateObject(IID_IMFMediaSource, (void**)&msrc);
    act = nullptr;
    if (FAILED(hr) || !msrc) {
        MFShutdown();
        if (comHere) CoUninitialize();
        err = L"ActivateObject failed: " + HrHex(hr);
        return false;
    }

    // QI контролов: отсутствие интерфейса — не ошибка (домен будет пустым).
    ATL::CComPtr<IAMVideoProcAmp> pa;
    ATL::CComPtr<IAMCameraControl> cc;
    msrc->QueryInterface(IID_IAMVideoProcAmp, (void**)&pa);
    msrc->QueryInterface(IID_IAMCameraControl, (void**)&cc);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        mediaSrc_ = std::move(msrc);
        procAmp_ = std::move(pa);
        camCtl_ = std::move(cc);
        mfUp_ = true;
        comUp_ = comHere;
    }
    return true;
}

void CameraControls::Attach(IMFMediaSource* mediaSrc)
{
    Detach();
    if (!mediaSrc) return;
    ATL::CComPtr<IAMVideoProcAmp> pa;
    ATL::CComPtr<IAMCameraControl> cc;
    mediaSrc->QueryInterface(IID_IAMVideoProcAmp, (void**)&pa);
    mediaSrc->QueryInterface(IID_IAMCameraControl, (void**)&cc);
    std::lock_guard<std::mutex> lock(mutex_);
    procAmp_ = std::move(pa);
    camCtl_ = std::move(cc);
}

void CameraControls::Detach()
{
    std::lock_guard<std::mutex> lock(mutex_);
    procAmp_ = nullptr;
    camCtl_ = nullptr;
}

void CameraControls::Close()
{
    ATL::CComPtr<IMFMediaSource> msrc;
    bool mfHere = false;
    bool comHere = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        procAmp_ = nullptr;
        camCtl_ = nullptr;
        msrc = std::move(mediaSrc_);
        mediaSrc_ = nullptr;
        mfHere = mfUp_;
        mfUp_ = false;
        comHere = comUp_;
        comUp_ = false;
    }
    // Shutdown источника — пока MF жива; IAM-указатели уже отпущены выше.
    if (msrc) {
        msrc->Shutdown();
        msrc = nullptr;
    }
    if (mfHere) MFShutdown();
    if (comHere) CoUninitialize();
}

bool CameraControls::IsOpen() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    return procAmp_ != nullptr || camCtl_ != nullptr;
}

bool CameraControls::FillOneLocked(CameraControlDomain domain, long propId,
                                   CameraControlDesc& desc) const
{
    desc = CameraControlDesc();
    desc.domain = domain;
    desc.id = propId;
    for (const PropRow& r : kProps)
        if (r.domain == domain && r.id == propId) { desc.name.assign(r.name); break; }
    if (desc.name.empty()) return false; // неизвестно спеке

    HRESULT hrRange = E_NOINTERFACE;
    if (domain == CameraControlDomain::ProcAmp) {
        if (!procAmp_) return true; // интерфейс отсутствует — unsupported, не ошибка
        hrRange = procAmp_->GetRange(propId, &desc.minValue, &desc.maxValue,
                                     &desc.step, &desc.defaultValue,
                                     &desc.capsFlags);
    } else {
        if (!camCtl_) return true;
        hrRange = camCtl_->GetRange(propId, &desc.minValue, &desc.maxValue,
                                    &desc.step, &desc.defaultValue,
                                    &desc.capsFlags);
    }
    if (FAILED(hrRange)) return true; // свойство не поддерживается драйвером
    desc.supported = true;

    HRESULT hrGet = E_FAIL;
    if (domain == CameraControlDomain::ProcAmp)
        hrGet = procAmp_->Get(propId, &desc.curValue, &desc.curFlags);
    else
        hrGet = camCtl_->Get(propId, &desc.curValue, &desc.curFlags);
    desc.getHr = hrGet;
    if (FAILED(hrGet)) {
        desc.curValue = desc.defaultValue; // диапазон есть, текущее не читается
        desc.curFlags = 0;
    }
    return true;
}

std::vector<CameraControlDesc> CameraControls::List() const
{
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<CameraControlDesc> out;
    if (!procAmp_ && !camCtl_) return out; // устройство не открыто — пусто
    out.reserve(kPropCount);
    for (int i = 0; i < kPropCount; i++) {
        CameraControlDesc d;
        FillOneLocked(kProps[i].domain, kProps[i].id, d);
        out.push_back(std::move(d));
    }
    return out;
}

bool CameraControls::Get(CameraControlDomain domain, long propId,
                         CameraControlDesc& desc) const
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!procAmp_ && !camCtl_) return false;
    return FillOneLocked(domain, propId, desc) && !desc.name.empty();
}

HRESULT CameraControls::Set(CameraControlDomain domain, long propId, long value,
                            long flags, long& appliedValue, long& appliedFlags)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!procAmp_ && !camCtl_) return E_HANDLE;
    return SetLocked(domain, propId, value, flags, appliedValue, appliedFlags);
}

HRESULT CameraControls::SetLocked(CameraControlDomain domain, long propId,
                                  long value, long flags,
                                  long& appliedValue, long& appliedFlags)
{
    appliedValue = value;
    appliedFlags = flags;

    CameraControlDesc cur;
    if (!FillOneLocked(domain, propId, cur)) return E_INVALIDARG;
    if (!cur.supported) return HRESULT_FROM_WIN32(ERROR_SET_NOT_FOUND);

    // Ручной Set поверх auto: драйвер проигнорирует значение, если не сбросить
    // режим в Manual. Явный auto-запрос (flags&Auto) пропускаем как есть.
    long wantFlags = flags;
    if (!(wantFlags & kFlagAuto) && (cur.curFlags & kFlagAuto))
        wantFlags = (wantFlags & ~kFlagAuto) | kFlagManual;
    if (wantFlags == 0) wantFlags = kFlagManual; // голый 0 драйверы не понимают

    HRESULT hr = E_FAIL;
    if (domain == CameraControlDomain::ProcAmp)
        hr = procAmp_->Set(propId, value, wantFlags);
    else
        hr = camCtl_->Set(propId, value, wantFlags);
    if (FAILED(hr)) return hr;

    // Возвращаем применённое (перечитываем Get).
    HRESULT hrGet = E_FAIL;
    if (domain == CameraControlDomain::ProcAmp)
        hrGet = procAmp_->Get(propId, &appliedValue, &appliedFlags);
    else
        hrGet = camCtl_->Get(propId, &appliedValue, &appliedFlags);
    if (FAILED(hrGet)) {
        appliedValue = value; // Set прошёл, перечитать не вышло — честно наружу
        appliedFlags = wantFlags;
    }
    return S_OK;
}
