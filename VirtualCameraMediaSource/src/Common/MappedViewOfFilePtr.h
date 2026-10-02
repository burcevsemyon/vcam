#pragma once
#include <windows.h>

// RAII-обёртка для view'а MapViewOfFile: закрывает в деструкторе, move-only
// (как ATL::CHandle / CComPtr — копирование запрещено). Attach/Detach для
// перехода на сырой указатель и обратно (глобальные/классовые поля).
// Освобождение безусловное: UnmapViewOfFile возвращает FALSE только если
// указатель не является валидным view'ем — результат не проверяем.
// operator& намеренно НЕ определён: &view должен оставаться адресом объекта.

namespace vcam {

class MappedViewOfFilePtr {
public:
    MappedViewOfFilePtr() = default;
    explicit MappedViewOfFilePtr(void* p) : p_(p) {}
    ~MappedViewOfFilePtr() { Close(); }

    MappedViewOfFilePtr(MappedViewOfFilePtr&& o) noexcept : p_(o.p_) { o.p_ = nullptr; }
    MappedViewOfFilePtr& operator=(MappedViewOfFilePtr&& o) noexcept
    {
        if (this != &o) { Close(); p_ = o.p_; o.p_ = nullptr; }
        return *this;
    }
    MappedViewOfFilePtr(const MappedViewOfFilePtr&) = delete;
    MappedViewOfFilePtr& operator=(const MappedViewOfFilePtr&) = delete;

    // Присваивание сырого указателя = владение им (Close старого + adopt).
    // nullptr подходит под void*; copy-assign остаётся deleted (точное
    // совпадение бьёт user-conv, v2 = v3 не скомпилируется).
    MappedViewOfFilePtr& operator=(void* p) { Close(); p_ = p; return *this; }

    bool Attach(void* p) { Close(); p_ = p; return p_ != nullptr; }
    void* Detach() { void* p = p_; p_ = nullptr; return p; }
    void Close() { if (p_) { UnmapViewOfFile(p_); p_ = nullptr; } }

    void* Get() const { return p_; }
    template <typename T> T* GetAs() const { return static_cast<T*>(p_); }
    explicit operator bool() const { return p_ != nullptr; }

private:
    void* p_ = nullptr;
};

} // namespace vcam
