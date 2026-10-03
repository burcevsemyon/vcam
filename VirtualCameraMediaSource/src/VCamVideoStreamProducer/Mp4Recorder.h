#pragma once

#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <atlbase.h>

#include <cstdint>
#include <string>

// Запись эфира в .mp4 (только видео, без аудио) через MF SinkWriter.
// Вход — ровно 720p-кадр v1 (packed 1280x720 BGRX top-down, stride 5120):
// FrameWriter::LastFrame720p() после успешного WriteOne, т.е. кадры С
// эффектами хоста, бит-в-бит как в эфире. Скейлер не нужен: v1 всегда 720p.
//
// Важно: входной RGB32 тракт SinkWriter→H.264 принимает как top-down
// (замерено: флип строк давал перевёрнутое видео). Кэш пишется как есть.
//
// Fail-open: любая ошибка записи — только LastError()/false; эфир (shared
// memory) это не затрагивает. Кодировщик медленнее реала — дроп кадров
// записи (счётчик FramesDropped), эфир не ждёт.
// Метки времени — wall-clock (QPC) от старта: монотонные, дропы дают честные
// гэпы, длительность файла = реальному времени записи.
// Stop() обязан вызываться при стопе/выходе (SinkWriter::Finalize), иначе
// mp4 битый. Вызывается только из worker-потока хоста.
class Mp4Recorder {
public:
    static constexpr uint32_t kW = 1280;
    static constexpr uint32_t kH = 720;
    static constexpr uint32_t kFps = 30;
    static constexpr uint32_t kStride = kW * 4; // 5120
    static constexpr size_t kFrameSize = (size_t)kW * kH * 4; // 3686400
    static constexpr uint64_t kFrameDur100ns = 10000000ull / kFps; // 333333
    static constexpr int64_t kBitrate = 8000000; // 8 Мбит/с, 720p30 с запасом
    // Долг кодировщика сверх этого — дропаем входящие кадры (эфир важнее).
    static constexpr double kDropDebtMs = 250.0;

    Mp4Recorder();
    ~Mp4Recorder();

    Mp4Recorder(const Mp4Recorder&) = delete;
    Mp4Recorder& operator=(const Mp4Recorder&) = delete;

    bool IsOpen() const { return open_; }
    const std::wstring& Path() const { return path_; }
    ULONGLONG StartTickMs() const { return startTickMs_; }
    uint64_t FramesWritten() const { return frames_; }
    uint64_t FramesDropped() const { return dropped_; }
    const std::wstring& LastError() const { return lastErr_; }

    // Открывает файл (каталог создаётся, существующий файл перезаписывается)
    // и готовит H.264-писатель. false = err заполнена, эфир продолжается.
    bool Start(const std::wstring& path, std::wstring& err);
    // Один 720p-кадр (kFrameSize байт). false = фатально (читай LastError,
    // вызывай Stop для финализации огрызка); дроп от долга — НЕ false.
    bool WriteFrame720p(const uint8_t* bgrxTopDown);
    // Finalize + освобождение. Идемпотентна; деструктор зовёт её же.
    void Stop();

private:
    static std::wstring HrHex(HRESULT hr);
    bool WriteSample(const uint8_t* bgrxTopDown);

    bool open_ = false;
    bool comUp_ = false;
    bool mfUp_ = false;
    std::wstring path_;
    std::wstring lastErr_;
    ATL::CComPtr<IMFSinkWriter> writer_;
    DWORD stream_ = 0;
    LONGLONG qpcFreq_ = 0;
    LONGLONG startQpc_ = 0;
    LONGLONG lastTs_ = -1; // 100-нс, строго монотонные
    ULONGLONG startTickMs_ = 0;
    uint64_t frames_ = 0;
    uint64_t dropped_ = 0;
    double debtMs_ = 0.0; // долг кодировщика, мс: +кодирование, −33.33/кадр
};
