#pragma once

#include "console.h"

[[noreturn]] inline void halt() {
    asm volatile("cli");

    printf("Halting...\n");

    while(true) {
        asm volatile("hlt");
    }
}