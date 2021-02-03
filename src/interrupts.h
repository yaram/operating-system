#pragma once

#include <stdint.h>

struct __attribute__((packed)) InterruptStackFrame {
    void *instruction_pointer;
    uint64_t code_segment;
    uint64_t cpu_flags;
    void *stack_pointer;
    uint64_t stack_segment;
};