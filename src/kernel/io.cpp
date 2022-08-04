#include "io.h"

void io_out(uint16_t port, uint8_t value) {
    asm volatile(
        "out %%al, %%dx"
        :
        : "d"(port), "a"(value)
    );
}

uint8_t io_in(uint16_t port) {
    uint8_t value;
    asm volatile(
        "in %%dx, %%al"
        : "=a"(value)
        : "d"(port)
    );

    return value;
}