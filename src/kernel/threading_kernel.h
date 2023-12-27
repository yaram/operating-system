#pragma once

#include "threading.h"

static void __attribute__((always_inline)) spinloop_pause() {
    asm volatile("pause");
}

static inline void acquire_lock(volatile bool *lock) {
    while(true) {        
        auto false_value = false; // Must be reset every iteration as __atomic_compare_exchange_n overwrites it
        if(__atomic_compare_exchange_n(lock, &false_value, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
            break;
        }

        spinloop_pause();
    }
}

static inline uint8_t get_processor_id() {
    uint32_t cpuid_value_a;
    uint32_t cpuid_value_b;
    uint32_t cpuid_value_c;
    uint32_t cpuid_value_d;
    asm volatile(
        "cpuid"
        : "=a"(cpuid_value_a), "=b"(cpuid_value_b), "=c"(cpuid_value_c), "=d"(cpuid_value_d)
        : "a"((uint32_t)1)
    );

    return cpuid_value_b >> 24;
}