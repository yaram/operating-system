#pragma once

#include "process.h"
#include "paging.h"

union GDTEntry {
    struct __attribute__((packed)) {
        uint16_t limit_low;
        uint32_t base_low: 24;
        bool accessed: 1;
        bool read_write: 1;
        bool direction_conforming: 1;
        bool executable: 1;
        bool type: 1;
        uint8_t privilege: 2;
        bool present: 1;
        uint8_t limit_high: 4;
        bool reserved: 1;
        bool long_mode: 1;
        bool size: 1;
        bool granularity: 1;
        uint8_t base_high;
    } normal_segment;

    struct __attribute__((packed)) {
        uint16_t limit_low;
        uint32_t base_low: 24;
        uint8_t type: 5;
        uint8_t privilege: 2;
        bool present: 1;
        uint8_t limit_high: 4;
        bool task_available: 1;
        uint8_t reserved: 2;
        bool granularity: 1;
        uint8_t base_mid;
    } task_state_segment_low;

    struct __attribute__((packed)) {
        uint32_t base_high;
        uint32_t reserved;
    } task_state_segment_high;
};

struct __attribute__((packed)) TSSEntry {
    uint32_t reserved_1;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved_2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved_3;
    uint16_t reserved_4;
    uint16_t iopb_offset;
};

// The addresses/sizes/offset in this file MUST remain in-sync with the ones in syscall.S

const auto processor_area_pages_start = kernel_pages_end;

const auto processor_area_memory_start = processor_area_pages_start * page_size;

const size_t processor_stack_size = 1024 * 16;

const size_t gdt_size = 7;

struct ProcessorArea {
    __attribute__((aligned(16))) uint8_t stack[processor_stack_size];

    GDTEntry gdt_entries[gdt_size];

    TSSEntry tss_entry;

    uint8_t processor_id;

    size_t physical_address;
    ProcessorArea *kernel_address;

    volatile uint32_t *apic_registers;

    Processes::Iterator current_process_iterator;

    bool in_syscall;
    bool preempt_during_syscall;
};

static_assert(processor_stack_size % 16 == 0, "Processor stack size of not 16-byte aligned");

const auto processor_area_size = sizeof(ProcessorArea);

const auto processor_area_page_count = divide_round_up(processor_area_size, page_size);