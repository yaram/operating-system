#include <stdint.h>
#include <stddef.h>
#include "printf.h"
#include "syscall.h"

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

void _putchar(char character) {
    syscall(SyscallType::DebugPrint, character, 0);
}

extern "C" [[noreturn]] void entry(size_t process_id, void *data, size_t data_size) {
    printf("Secondary process started!\n");

    if(data_size != sizeof(size_t)) {
        printf("Error: Invalid data size for secondary process. Expected %zu, got %zu\n", sizeof(size_t), data_size);

        exit();
    }

    auto number_to_print = *(size_t*)data;

    for(size_t i = 0; i < 10; i += 1) {
        printf("Test %zu\n", number_to_print);

        for(size_t j = 0; j < 10000; j += 1) {
            asm volatile("");
        }
    }

    exit();
}