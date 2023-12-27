#pragma once

#include "process.h"
#include "paging.h"

union __attribute__((aligned(16))) APICRegister {
    uint32_t value;
};

struct APICRegisters {
    volatile APICRegister reserved_1[2];

    volatile APICRegister lapic_id;
    volatile APICRegister lapic_version;

    volatile APICRegister reserved_2[4];

    volatile APICRegister task_priority;
    volatile APICRegister arbitration_priority;
    volatile APICRegister processor_priority;
    volatile APICRegister end_of_interrupt;
    volatile APICRegister remote_read;
    volatile APICRegister logical_destination;
    volatile APICRegister destination_format;
    volatile APICRegister spurious_interrupt_vector;

    volatile APICRegister in_service[8];
    volatile APICRegister trigger_mode[8];
    volatile APICRegister interrupt_request[8];

    volatile APICRegister error_status;

    volatile APICRegister reserved_3[6];

    volatile APICRegister lvt_corrected_machine_check_interrupt;
    volatile APICRegister interrupt_command_lower;
    volatile APICRegister interrupt_command_upper;
    volatile APICRegister lvt_timer;
    volatile APICRegister lvt_thermal_sensor;
    volatile APICRegister lvt_performance_monitoring_counters;
    volatile APICRegister lvt_lint0;
    volatile APICRegister lvt_lint1;
    volatile APICRegister lvt_error;
    volatile APICRegister timer_initial_count;
    volatile APICRegister timer_current_count;

    volatile APICRegister reserved_4[4];

    volatile APICRegister timer_divide_configuration;
    volatile APICRegister reserved_5;
};

static_assert(sizeof(APICRegisters) == 0x400, "APIC register struct is incorrect size");

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

const auto user_processor_areas_pages_start = kernel_pages_end;

const auto user_processor_areas_memory_start = user_processor_areas_pages_start * page_size;

const size_t processor_stack_size = 1024 * 16;

const size_t gdt_size = 7;

struct ProcessorArea {
    // Needed for GS register crazyness in syscall.S
    size_t user_address;

    __attribute__((aligned(16))) uint8_t stack[processor_stack_size];

    GDTEntry gdt_entries[gdt_size];

    TSSEntry tss_entry;

    APICRegisters *apic_registers;

    Processes::Iterator current_process_iterator;
    ProcessThreads::Iterator current_thread_iterator;

    bool in_syscall_or_user_exception;
    bool preempt_during_syscall_or_user_exception;
};

static_assert(processor_stack_size % 16 == 0, "Processor stack size not 16-byte aligned");

const auto processor_area_size = sizeof(ProcessorArea);

void send_kernel_page_tables_update(size_t pages_start, size_t page_count);

inline void send_kernel_page_tables_update_memory(void *memory_start, size_t size) {
    auto pages_start = (size_t)memory_start / page_size;
    auto pages_end = divide_round_up((size_t)memory_start + size, page_size);

    auto page_count = pages_end - pages_start;

    send_kernel_page_tables_update(pages_start, page_count);
}