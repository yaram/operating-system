#pragma once

#include <stddef.h>
#include <stdint.h>

static inline void fill_memory(void *address, size_t length, uint8_t value) {
    auto temp_address = address;

    asm volatile(
        "rep stosb"
        : "=D"(temp_address), "=c"(length)
        : "D"(temp_address), "a"((uint8_t)value), "c"(length)
    );
}

static inline void copy_memory(const void *source_address, void *destination_address, size_t length) {
    auto temp_destination_address = destination_address;

    asm volatile(
        "rep movsb"
        : "=S"(source_address), "=D"(temp_destination_address), "=c"(length)
        : "S"(source_address), "D"(temp_destination_address), "c"(length)
    );
}