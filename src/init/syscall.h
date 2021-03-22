#pragma once

#include "syscalls.h"

inline size_t syscall(SyscallType syscall_type, size_t parameter_1, size_t parameter_2, size_t *return_2) {
    size_t return_1;
    asm volatile(
        "syscall"
        : "=b"(return_1), "=d"(*return_2), "=S"(parameter_2)
        : "b"(syscall_type), "d"(parameter_1), "S"(parameter_2)
        : "rax", "rcx", "r11"
    );

    return return_1;
}

inline size_t syscall(SyscallType syscall_type, size_t parameter_1, size_t parameter_2) {
    size_t return_2;
    return syscall(syscall_type, parameter_1, parameter_2, &return_2);
}

[[noreturn]] inline void exit() {
    syscall(SyscallType::Exit, 0, 0);

    while(true);
}