#pragma once

#include <stdint.h>
#include "interrupts.h"

struct __attribute__((aligned(16))) ProcessStackFrame {
    // Base integer registers
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rbp;

    uint8_t padding_before[8];

    // x87/MMX/SSE registers, must be 16-byte aligned from the start and end of the struct
    uint8_t flags[32];
    uint8_t mmx_x87[8][16];
    uint8_t sse[16][16];
    uint8_t reserved[96];

    uint8_t padding_after[8];

    InterruptStackFrame interrupt_frame;
};

struct Process {
    size_t logical_pages_start;
    size_t page_count;

    size_t pml4_table_physical_address;

    size_t id;

    ProcessStackFrame frame;
};