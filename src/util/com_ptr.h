#pragma once
#include <windows.h>
#include <memory>
#include <type_traits>

template<typename T>
class ComPtr {
    T* ptr_ = nullptr;
public:
    ComPtr() = default;
    ComPtr(std::nullptr_t) {}
    explicit ComPtr(T* p, bool add_ref = false) : ptr_(p) { if (add_ref && ptr_) ptr_->AddRef(); }
    ComPtr(const ComPtr& other) : ptr_(other.ptr_) { if (ptr_) ptr_->AddRef(); }
    ComPtr(ComPtr&& other) noexcept : ptr_(other.ptr_) { other.ptr_ = nullptr; }
    ~ComPtr() { reset(); }

    ComPtr& operator=(const ComPtr& other) {
        if (std::addressof(other) != this) { reset(); ptr_ = other.ptr_; if (ptr_) ptr_->AddRef(); }
        return *this;
    }
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (std::addressof(other) != this) { reset(); ptr_ = other.ptr_; other.ptr_ = nullptr; }
        return *this;
    }
    ComPtr& operator=(T* p) {
        reset(); ptr_ = p; if (ptr_) ptr_->AddRef();
        return *this;
    }

    void reset() { if (ptr_) { ptr_->Release(); ptr_ = nullptr; } }
    T* get() const { return ptr_; }
    T** operator&() { reset(); return &ptr_; }
    T* operator->() const { return ptr_; }
    explicit operator bool() const { return ptr_ != nullptr; }

    void Attach(T* p) { reset(); ptr_ = p; }
    T* Detach() { T* p = ptr_; ptr_ = nullptr; return p; }

    template<typename U>
    HRESULT As(ComPtr<U>& out) const {
        if (!ptr_) return E_POINTER;
        return ptr_->QueryInterface(__uuidof(U), (void**)out.ReleaseAndGetAddress());
    }

    T** GetAddress() { return &ptr_; }
    T** ReleaseAndGetAddress() { reset(); return &ptr_; }
};

class ComInit {
    HRESULT hr_;
public:
    explicit ComInit(DWORD mode = COINIT_MULTITHREADED) : hr_(CoInitializeEx(nullptr, mode)) {}
    ~ComInit() { if (SUCCEEDED(hr_)) CoUninitialize(); }
    bool Succeeded() const { return SUCCEEDED(hr_); }
};


