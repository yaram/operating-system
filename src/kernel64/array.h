#pragma once

#include <stddef.h>
#include "halt.h"

template <typename T>
struct Array {
    T *data;
    size_t length;

    inline T &operator [](size_t index) {
#if !defined(NDEBUG)
        if(index >= length) {
            printf("FATAL ERROR: Array overrun. Length was %zu and index was %zu\n", length, index);

            halt();
        }
#endif

        return data[index];
    }
};

template <typename T>
struct ConstArray {
    const T *data;
    size_t length;

    ConstArray() = default;

    ConstArray(const T *data, size_t length) {
        this->data = data;
        this->length = length;
    }

    ConstArray(const Array<T> &array) {
        data = array.data;
        length = array.length;
    }

    inline const T &operator [](size_t index) {
#if !defined(NDEBUG)
        if(index >= length) {
            printf("FATAL ERROR: Array overrun. Length was %zX and index was %zX\n", length, index);

            halt();
        }
#endif

        return data[index];
    }
};