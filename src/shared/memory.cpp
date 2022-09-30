#include "memory.h"

extern "C" void *memset(void *destination, int value, size_t count) {
    fill_memory(destination, count, (uint8_t)value);

    return destination;
}

extern "C" void *memcpy(void *destination, const void *source, size_t count) {
    copy_memory(source, destination, count);

    return destination;
}