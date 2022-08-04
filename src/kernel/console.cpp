#include "console.h"
#include <stddef.h>
#include <stdint.h>
#include "io.h"

static void serial_write(uint16_t base_port, uint8_t value) {
    while((io_in(base_port + 5) & 0x20) == 0) {}

    io_out(base_port, value);
}

void setup_console() {
    io_out(0x3F8 + 1, 0x00);
    io_out(0x3F8 + 3, 0x80);
    io_out(0x3F8 + 0, 0x0C);
    io_out(0x3F8 + 1, 0x00);
    io_out(0x3F8 + 3, 0x03);
    io_out(0x3F8 + 2, 0xC7);
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