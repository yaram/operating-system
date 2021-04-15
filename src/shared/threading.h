#pragma once

template <typename T>
static inline bool compare_and_swap(volatile T *value, T expected_value, T new_value) {
    return __atomic_compare_exchange_n(value, &expected_value, new_value, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}