#include "console.h"
#include <stddef.h>
#include <stdint.h>

const size_t column_count = 80;
const size_t line_count = 25;

const auto vga_memory = (volatile uint8_t *)0xB8000;

uint32_t column = 0;
uint32_t line = 0;

void clear_console() {
    for(size_t i = 0; i < line_count * column_count * 2; i += 2) {
        vga_memory[i] = ' ';
    }
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
        return;
    }

    vga_memory[line * column_count * 2 + column * 2] = character;

    column += 1;
}