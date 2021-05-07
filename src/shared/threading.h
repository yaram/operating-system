#pragma once

template <typename T>
static inline bool compare_and_swap(volatile T *value, T expected_value, T new_value) {
    return __atomic_compare_exchange_n(value, &expected_value, new_value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

template <typename T>
static inline T atomic_add(volatile T *value, T addend) {
    return __atomic_add_fetch(value, addend, __ATOMIC_SEQ_CST);
}