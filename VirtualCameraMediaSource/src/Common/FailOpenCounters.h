#pragma once

// Счётчики fail-open деградаций (backlog observability.md P0.2).
// Каждый тихий fallback инкрементит свой счётчик; P0.3 экспортирует их в `status`.
// Header-only + atomics — доступен из любого модуля (host / CLI / ProducerCore /
// MediaSource). Функция-локальный static в inline-функции = один экземпляр на модуль
// (EXE или DLL): каждый процесс считает свои деградации независимо.

#include <atomic>
#include <cstdint>

namespace vcam {

enum class FailOpen {
    SettingsBrokenJson,    // битый/нераспознанный settings.json -> дефолт
    HotkeyBusy,            // хоткей не зарегистрирован (занят/ошибка RegisterHotKey)
    RecordStartFailed,     // запись эфира не стартовала
    SourceOpenFailed,      // источник кадров не открылся
    StaleTransientRemoved, // удалён stale-транзиент (record_command/state/hotkey)
    ControlUnavailable,    // контрол камеры недоступен (E_PROP_ID_UNSUPPORTED)
    Count
};

// Счётчик конкретного типа (инициализируется нулём, thread-safe).
inline std::atomic<int64_t>& FailOpenCounter(FailOpen k)
{
    static std::atomic<int64_t> counters[static_cast<size_t>(FailOpen::Count)] = {};
    return counters[static_cast<size_t>(k)];
}

// Инкремент счётчика деградации (relaxed — порядок не важен, важна атомарность).
inline void IncFailOpen(FailOpen k)
{
    FailOpenCounter(k).fetch_add(1, std::memory_order_relaxed);
}

// Текущее значение счётчика (для P0.3 / status).
inline int64_t GetFailOpen(FailOpen k)
{
    return FailOpenCounter(k).load(std::memory_order_relaxed);
}

} // namespace vcam
