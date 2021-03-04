#pragma once

#include <stdint.h>
#include "interrupts.h"
#include "bucket_array.h"

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
    uint8_t other_flags[24];
    uint32_t mxcsr;
    uint32_t mxcsr_mask;
    uint8_t mmx_x87[8][16];
    uint8_t sse[16][16];
    uint8_t reserved[96];

    uint8_t padding_after[8];

    InterruptStackFrame interrupt_frame;
};

struct ProcessPageMapping {
    size_t logical_pages_start;
    size_t page_count;

    bool is_owned;
};

using ProcessPageMappings = BucketArray<ProcessPageMapping, 16>;

struct Process {
    size_t logical_pages_start;
    size_t page_count;

    size_t pml4_table_physical_address;

    size_t id;

    ProcessPageMappings mappings;

    ProcessStackFrame frame;
};

using Processes = BucketArray<Process, 4>;

extern Processes global_processes;

bool create_process_from_elf(
    uint8_t *elf_binary,
    uint8_t *bitmap_entries,
    size_t bitmap_size,
    Processes *processes,
    Process **result_processs,
    Processes::Iterator *result_process_iterator
);
bool destroy_process(Processes::Iterator iterator, uint8_t *bitmap_entries, size_t bitmap_size);
bool register_process_mapping(
    Process *process,
    size_t logical_pages_start,
    size_t page_count,
    bool is_owned,
    uint8_t *bitmap_entries,
    size_t bitmap_size
);