#include "console.h"
#include <stddef.h>
#include <stdint.h>

const size_t column_count = 80;
const size_t line_count = 25;

const auto vga_memory = (volatile uint8_t *)0xB8000;

uint32_t column = 0;
uint32_t line = 0;

static void out(uint16_t port, uint8_t value) {
    __asm volatile(
        "out %%al, %%dx"
        :
        : "d"(port), "a"(value)
    );
}

static void move_cursor(uint32_t column, uint32_t line) {
    auto index = line * column_count + column;

    out(0x03D4, 0x0F);
    out(0x03D5, (uint8_t)index);
    out(0x03D4, 0x0E);
    out(0x03D5, (uint8_t)(index >> 8));
}

void clear_console() {
    for(size_t i = 0; i < line_count * column_count * 2; i += 2) {
        vga_memory[i] = ' ';
    }

    move_cursor(0, 0);
}

void _putchar(char character) {
    if(character >= 32 && column == column_count) {
        column = 0;
        line += 1;
    } else if(character == '\n') {
        column = 0;
        line += 1;
    } else if(character < 32) {
        return;
    }

    if(line == line_count) {
        for(size_t moving_line = 0; moving_line < line_count - 1; moving_line += 1) {
            for(size_t moving_column = 0; moving_column < column_count; moving_column += 1) {
                vga_memory[moving_line * column_count * 2 + moving_column * 2] = vga_memory[(moving_line + 1) * column_count * 2 + moving_column * 2];
            }
        }

        for(size_t moving_column = 0; moving_column < column_count; moving_column += 1) {
            vga_memory[(line_count - 1) * column_count * 2 + moving_column * 2] = ' ';
        }

        line -= 1;
    }

    if(character == '\n') {
        move_cursor(column, line);

        return;
    }

    vga_memory[line * column_count * 2 + column * 2] = character;

    column += 1;

    move_cursor(column, line);
}