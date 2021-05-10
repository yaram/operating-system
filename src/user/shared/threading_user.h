#pragma once

#include "threading.h"
#include "syscall.h"

static inline void acquire_lock(volatile bool *lock) {
    // Spinlock for 10 iterators before relinquishing process time, and repeating
    while(true) {
        for(size_t i = 0; i < 10; i += 1) {
            auto false_value = false; // Must be reset every iteration as __atomic_compare_exchange_n overwrites it
            if(__atomic_compare_exchange_n(lock, &false_value, true, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST)) {
                return;
            }

            asm volatile("pause");
        }

        syscall(SyscallType::RelinquishTime, 0, 0);
    }
}