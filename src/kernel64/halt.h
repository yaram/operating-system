#pragma once

#include "console.h"

[[noreturn]] inline void halt() {
    printf("Halting...\n");

    while(true) {
        asm volatile("hlt");
    }
}