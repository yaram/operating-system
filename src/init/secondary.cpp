#include <stdint.h>
#include <stddef.h>
#include "printf.h"
#include "syscalls.h"

extern "C" void *memset(void *destination, int value, size_t count) {
    auto temp_destination = destination;

    asm volatile(
        "rep stosb"
        : "=D"(temp_destination), "=c"(count)
        : "D"(temp_destination), "a"((uint8_t)value), "c"(count)
    );

    return destination;
}

extern "C" void *memcpy(void *destination, const void *source, size_t count) {
    auto temp_destination = destination;

    asm volatile(
        "rep movsb"
        : "=S"(source), "=D"(temp_destination), "=c"(count)
        : "S"(source), "D"(temp_destination), "c"(count)
    );

    return destination;
}

inline size_t syscall(SyscallType syscall_type, size_t parameter, size_t *return_2) {
    size_t return_1;
    asm volatile(
        "syscall"
        : "=b"(return_1), "=d"(*return_2)
        : "b"(syscall_type), "d"(parameter)
        : "rax", "rcx", "r11"
    );

    return return_1;
}

inline size_t syscall(SyscallType syscall_type, size_t parameter) {
    size_t return_2;
    return syscall(syscall_type, parameter, &return_2);
}

[[noreturn]] inline void exit() {
    syscall(SyscallType::Exit, 0);

    while(true);
}

void _putchar(char character) {
    syscall(SyscallType::DebugPrint, character);
}

extern "C" [[noreturn]] void entry() {
    printf("Secondary process started!\n");

    for(size_t i = 0; i < 10; i += 1) {
        printf("Test\n");

        for(size_t j = 0; j < 10000; j += 1) {
            asm volatile("");
        }
    }

    exit();
}