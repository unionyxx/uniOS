#pragma once
#include <kernel/mm/heap.h>
#include <stddef.h>

namespace kstd {

template<typename T>
class unique_ptr {
    T* ptr;
public:
    explicit unique_ptr(T* p = nullptr) : ptr(p) {}
    ~unique_ptr() { if (ptr) free(ptr); }
    
    unique_ptr(const unique_ptr&) = delete;
    unique_ptr& operator=(const unique_ptr&) = delete;
    
    unique_ptr(unique_ptr&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }
    
    unique_ptr& operator=(unique_ptr&& other) noexcept {
        if (this != &other) {
            if (ptr) free(ptr);
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }
    
    T* get() const { return ptr; }
    T& operator*() const { return *ptr; }
    T* operator->() const { return ptr; }
    T& operator[](size_t i) const { return ptr[i]; }
    explicit operator bool() const { return ptr != nullptr; }
    
    void reset(T* p = nullptr) {
        if (ptr) free(ptr);
        ptr = p;
    }

    T* release() {
        T* p = ptr;
        ptr = nullptr;
        return p;
    }
};

template<typename T>
class unique_ptr<T[]> {
    T* ptr;
public:
    explicit unique_ptr(T* p = nullptr) : ptr(p) {}
    ~unique_ptr() { if (ptr) free(ptr); }
    
    unique_ptr(const unique_ptr&) = delete;
    unique_ptr& operator=(const unique_ptr&) = delete;
    
    unique_ptr(unique_ptr&& other) noexcept : ptr(other.ptr) {
        other.ptr = nullptr;
    }
    
    unique_ptr& operator=(unique_ptr&& other) noexcept {
        if (this != &other) {
            if (ptr) free(ptr);
            ptr = other.ptr;
            other.ptr = nullptr;
        }
        return *this;
    }
    
    T* get() const { return ptr; }
    T& operator[](size_t i) const { return ptr[i]; }
    explicit operator bool() const { return ptr != nullptr; }
    
    void reset(T* p = nullptr) {
        if (ptr) free(ptr);
        ptr = p;
    }

    T* release() {
        T* p = ptr;
        ptr = nullptr;
        return p;
    }
};

template<typename T, typename... Args>
unique_ptr<T> make_unique(Args&&... args) {
    T* ptr = (T*)malloc(sizeof(T));
    if (ptr) new(ptr) T(static_cast<Args&&>(args)...);
    return unique_ptr<T>(ptr);
}

template<typename T>
class KBuffer {
    T* ptr;
    size_t size;
public:
    explicit KBuffer(size_t s) : size(s) {
        ptr = (T*)malloc(s * sizeof(T));
    }
    ~KBuffer() {
        if (ptr) free(ptr);
    }
    
    T* get() { return ptr; }
    const T* get() const { return ptr; }
    size_t get_size() const { return size; }
    
    explicit operator bool() const { return ptr != nullptr; }
    T& operator[](size_t index) { return ptr[index]; }
};

} // namespace kstd
