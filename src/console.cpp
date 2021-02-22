#include "console.h"
#include <stddef.h>
#include <stdint.h>

static void out(uint16_t port, uint8_t value) {
    asm volatile(
        "out %%al, %%dx"
        :
        : "d"(port), "a"(value)
    );
}

void clear_console() {}

void _putchar(char character) {
    if(character == '\n') {
        out(0x3F8, '\r');
    }

    out(0x3F8, character);
}