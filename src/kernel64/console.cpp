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

static uint8_t in(uint16_t port) {
    uint8_t value;
    asm volatile(
        "in %%dx, %%al"
        : "=a"(value)
        : "d"(port)
    );

    return value;
}

static void serial_write(uint16_t base_port, uint8_t value) {
    while((in(base_port + 5) & 0x20) == 0) {}

    out(base_port, value);
}

void setup_console() {
    out(0x3F8 + 1, 0x00);
    out(0x3F8 + 3, 0x80);
    out(0x3F8 + 0, 0x0C);
    out(0x3F8 + 1, 0x00);
    out(0x3F8 + 3, 0x03);
    out(0x3F8 + 2, 0xC7);
}

void _putchar(char character) {
    if(character == '\n') {
        serial_write(0x3F8, '\r');
    }

    serial_write(0x3F8, character);
}

void putchar(char character) {
    _putchar(character);
}