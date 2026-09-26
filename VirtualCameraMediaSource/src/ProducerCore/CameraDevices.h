#pragma once

#include <string>
#include <vector>

struct IMFActivate;

// Описание физической камеры (видеозахват Media Foundation).
struct CameraDeviceInfo {
    // MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK (symlink;
    // в SDK это именно он — VIDCAP_GUID оказывается значением типа источника)
    std::wstring id;
    std::wstring name; // MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME (VT_LPWSTR)
};

// Перечисление доступных камер. Пусто при ошибке/отсутствии устройств.
// Гарантирует COM/MFStartup внутри (парный MFShutdown), безопасно вызывать
// повторно и из любого потока — внешняя инициализация не требуется.
std::vector<CameraDeviceInfo> EnumerateCameraDevices();

// Как EnumerateCameraDevices + матчинг (точный id (i-cmp) -> точное имя ->
// подстрока имени): возвращает activate НАЙДЕННОЙ камеры с передачей владения
// (caller обязан Release()) + её описание. nullptr = не найдено/ошибка.
// MFStartup гарантируется внутри (парный MFShutdown).
IMFActivate* OpenCameraActivate(const std::wstring& id, const std::wstring& name,
                                CameraDeviceInfo& outInfo);
