#pragma once

#include <stdint.h>
#include "paging.h"
#include "interrupts.h"

struct __attribute__((packed)) ProcessStackFrame {
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

    InterruptStackFrame interrupt_frame;
};

struct Process {
    __attribute__((aligned(page_size)))
    PageTableEntry pml4_table[page_table_length];

    ProcessStackFrame frame;
};

const size_t process_count = 2;