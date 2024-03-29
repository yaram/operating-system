#pragma once

#include <stdint.h>
#include "interrupts.h"
#include "bucket_array.h"
#include "array.h"

// Positions of members in this struct are VERY IMPORTANT and relied on by assembly code and the architecture
struct __attribute__((aligned(16))) ThreadStackFrame {
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

    uint8_t padding[8];

    // x87/MMX/SSE registers
    uint8_t x87_flags[24];
    uint32_t mxcsr;
    uint32_t mxcsr_mask;
    uint8_t mmx_x87[8][16];
    uint8_t sse[16][16];
    uint8_t reserved[96];

    InterruptStackFrame interrupt_frame;
};

struct DebugCodeSection {
    size_t memory_start;
    size_t size;

    const static size_t name_buffer_length = 32;
    char name_buffer[name_buffer_length];
    size_t name_length;
};

using DebugCodeSections = BucketArray<DebugCodeSection, 16>;

struct ProcessPageMapping {
    size_t logical_pages_start;
    size_t page_count;

    bool is_shared;
    bool is_owned;
};

using ProcessPageMappings = BucketArray<ProcessPageMapping, 16>;

struct ProcessThread {
    ThreadStackFrame frame;

    bool is_resident;
    uint8_t resident_processor_id;

    bool is_ready;
};

using ProcessThreads = BucketArray<ProcessThread, 4>;

struct Process {
    size_t pml4_table_physical_address;

    size_t id;

    ProcessPageMappings mappings;

    DebugCodeSections debug_code_sections;

    ProcessThreads threads;

    bool is_ready;
};

using Processes = BucketArray<Process, 4>;

extern Processes global_processes;

enum struct CreateProcessFromELFResult {
    Success,
    OutOfMemory,
    InvalidELF
};

CreateProcessFromELFResult create_process_from_elf(
    uint8_t *elf_binary,
    size_t elf_binary_size,
    void *data,
    size_t data_size,
    Array<uint8_t> bitmap,
    size_t processor_area_count,
    size_t processor_areas_physical_memory_start,
    Processes *processes,
    Process **result_processs,
    Processes::Iterator *result_process_iterator
);
bool destroy_process(Processes::Iterator iterator, Array<uint8_t> bitmap);
bool register_process_mapping(
    Process *process,
    size_t logical_pages_start,
    size_t page_count,
    bool is_shared,
    bool is_owned,
    Array<uint8_t> bitmap
);