#include "console.h"
#include <stddef.h>
#include <stdint.h>

template <typename T>
static void unsigned_int_to_string(char *buffer, T value, T radix) {
    if(value == 0) {
        buffer[0] = '0';
        buffer[1] = '\0';

        return;
    }

    const char digits[] = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F' };

    size_t length = 0;
    while(value != 0) {
        auto digit_value = value % radix;

        buffer[length] = digits[digit_value];

        value /= radix;
        length += 1;
    }

    for(size_t i = 0; i < length / 2; i += 1) {
        auto temp = buffer[i];
        buffer[i] = buffer[length - i - 1];
        buffer[length - i - 1] = temp;
    }

    buffer[length] = '\0';
}

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

static void print_character(char character) {
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

static void print_string(const char *string) {
    while(*string != 0) {
        print_character(*string);

        string += 1;
    }
}

void vprintf(const char *format, va_list Args) {
    const auto vga_memory = (volatile uint8_t *)0xB8000;

    while(*format != 0) {
        if(*format == '%') {
            format += 1;

            switch(*format) {
                case '%': {
                    print_character('%');
                } break;

                case 's': {
                    auto string = va_arg(Args, const char*);

                    print_string(string);
                } break;

                case 'd': {
                    auto value = va_arg(Args, int);

                    if(value < 0) {
                        value = -value;
                    }

                    char buffer[32];
                    unsigned_int_to_string(buffer, value, 10);

                    print_string(buffer);
                } break;

                case 'u': {
                    auto value = va_arg(Args, unsigned int);

                    char buffer[32];
                    unsigned_int_to_string(buffer, value, 10u);

                    print_string(buffer);
                } break;

                case 'x': {
                    auto value = va_arg(Args, unsigned int);

                    char buffer[32];
                    unsigned_int_to_string(buffer, value, 16u);

                    print_string(buffer);
                } break;

                case '|': {
                    auto value = va_arg(Args, size_t);

                    char buffer[32];
                    unsigned_int_to_string(buffer, value, (size_t)16);

                    print_string(buffer);
                } break;

                default: {
                    print_character('%');
                    print_character(*format);
                } break;
            }
        } else {
            print_character(*format);
        }

        format += 1;
    }
}


void printf(const char *format, ...) {
    va_list args;
    va_start(args, format);

    vprintf(format, args);

    va_end(args);
}