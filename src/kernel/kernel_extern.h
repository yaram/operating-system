#pragma once

#include <stddef.h>

struct BootstrapMemoryMapEntry {
    size_t physical_address;
    size_t length;
    bool available;
};

struct BootstrapSpace {
    size_t acpi_table_physical_address;

    const static size_t memory_map_max_size = 1024;

    size_t memory_map_size;
    BootstrapMemoryMapEntry memory_map[memory_map_max_size];

    const static size_t stack_size = 16 * 1024;
    __attribute__((aligned(16)))
    uint8_t stack[stack_size];
};

const static size_t bootstrap_space_address = 0x100000;

using KernelMain = void (bool is_first_entry);