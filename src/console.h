#pragma once

#include <stdarg.h>

void clear_console();
void vprintf(const char *format, va_list Args);
void printf(const char *format, ...);