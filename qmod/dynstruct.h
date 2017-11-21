#pragma once
#include "types.h"

// the point of this is to support multiple versions of the target binary which
// might have different offsets.  currently not necessary.

template <size_t offset, typename T>
struct field_at {
    field_at(field_at &) = delete;
    inline T *_ptr() {
        return (T *)((char *)this + offset);
    }
    inline T *operator&() {
        return _ptr();
    }
    inline operator T&() {
        return *_ptr();
    }
    inline T &operator=(const T &other) {
        T *p = _ptr();
        *p = other;
        return *p;
    }
    inline T &operator()() {
        return *_ptr();
    }
    inline T &operator->() {
        return *_ptr();
    }
    #define FORWARD_OP(op) \
        template <typename... Args> \
        inline auto&& operator op(Args&&... args) { \
            return _ptr()->operator op(forward<Args>(args)...); \
        }

    FORWARD_OP(+);
    FORWARD_OP(-);
    FORWARD_OP(*);
    FORWARD_OP(/);
    FORWARD_OP([]);
    static constexpr size_t OFFSET = offset;
};

template <typename T>
struct ds_arrayptr {
    constexpr ds_arrayptr(char *_ptr) : _ptr(_ptr) {}
    T &operator*() {
        return *(T *)_ptr;
    }
    constexpr ds_arrayptr<T> operator+(size_t i) {
        return ds_arrayptr(_ptr + i * T::SIZE);
    }
    constexpr ds_arrayptr<T> operator-(size_t i) {
        return ds_arrayptr(_ptr - i * T::SIZE);
    }
    T &operator[](size_t i) {
        return *(*this + i);
    }
    ds_arrayptr<T> &operator++() {
        _ptr += T::SIZE;
        return *this;
    }
    ds_arrayptr<T> &operator--() {
        _ptr -= T::SIZE;
        return *this;
    }
    ds_arrayptr<T> operator++(int) {
        ds_arrayptr<T> old = *this;
        _ptr += T::SIZE;
        return old;
    }
    ds_arrayptr<T> operator--(int) {
        ds_arrayptr<T> old = *this;
        _ptr -= T::SIZE;
        return old;
    }

    char *_ptr;
};
