#pragma once

#include <utility>

namespace kopt
{
    template <typename T>
    class ComPtr
    {
    public:
        ComPtr() = default;
        ComPtr(std::nullptr_t) {}
        explicit ComPtr(T* pointer) : pointer_(pointer) {}
        ComPtr(const ComPtr& other) : pointer_(other.pointer_)
        {
            if (pointer_ != nullptr) pointer_->AddRef();
        }
        ComPtr(ComPtr&& other) noexcept : pointer_(std::exchange(other.pointer_, nullptr)) {}
        ~ComPtr() { reset(); }

        ComPtr& operator=(const ComPtr& other)
        {
            if (this == &other) return *this;
            T* incoming = other.pointer_;
            if (incoming != nullptr) incoming->AddRef();
            reset();
            pointer_ = incoming;
            return *this;
        }
        ComPtr& operator=(ComPtr&& other) noexcept
        {
            if (this == &other) return *this;
            reset();
            pointer_ = std::exchange(other.pointer_, nullptr);
            return *this;
        }

        [[nodiscard]] T* get() const noexcept { return pointer_; }
        [[nodiscard]] T** put() noexcept
        {
            reset();
            return &pointer_;
        }
        [[nodiscard]] T* const* address() const noexcept { return &pointer_; }
        T* operator->() const noexcept { return pointer_; }
        explicit operator bool() const noexcept { return pointer_ != nullptr; }
        bool operator==(std::nullptr_t) const noexcept { return pointer_ == nullptr; }
        bool operator!=(std::nullptr_t) const noexcept { return pointer_ != nullptr; }

        void reset(T* pointer = nullptr) noexcept
        {
            if (pointer_ != nullptr) pointer_->Release();
            pointer_ = pointer;
        }

    private:
        T* pointer_{};
    };
}
