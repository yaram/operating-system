#pragma once

#include "threading.h"

static inline void acquire_lock(volatile bool *lock) {
    while(true) {        
        auto false_value = false; // Must be reset every iteration as __atomic_compare_exchange_n overwrites it
        if(__atomic_compare_exchange_n(lock, &false_value, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            break;
        }

        asm volatile("pause");
    }
}