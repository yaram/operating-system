#pragma once

#include <stddef.h>
#include <stdint.h>
#include "memory_map.h"

struct Process;

const size_t page_size = 4096;

struct __attribute__((packed)) PML4Entry {
    bool present: 1;
    bool write_allowed: 1;
    bool user_mode_allowed: 1;
    bool write_through: 1;
    bool cache_disable: 1;
    bool accessed: 1;
    bool _ignored_0: 1;
    bool _reserved_0: 1;
    uint8_t _ignored_1: 4;
    size_t pdp_table_page_address: 40;
    uint16_t _ignored_2: 11;
    bool execute_disable: 1;
};

struct __attribute__((packed)) PDPEntry {
    bool present: 1;
    bool write_allowed: 1;
    bool user_mode_allowed: 1;
    bool write_through: 1;
    bool cache_disable: 1;
    bool accessed: 1;
    bool _ignored_0: 1;
    bool page_size: 1;
    uint8_t _ignored_1: 4;
    size_t pd_table_page_address: 40;
    uint16_t _ignored_2: 11;
    bool execute_disable: 1;
};

struct __attribute__((packed)) PDEntry {
    bool present: 1;
    bool write_allowed: 1;
    bool user_mode_allowed: 1;
    bool write_through: 1;
    bool cache_disable: 1;
    bool accessed: 1;
    bool _ignored_0: 1;
    bool page_size: 1;
    uint8_t _ignored_1: 4;
    size_t page_table_page_address: 40;
    uint16_t _ignored_2: 11;
    bool execute_disable: 1;
};

struct __attribute__((packed)) PageEntry {
    bool present: 1;
    bool write_allowed: 1;
    bool user_mode_allowed: 1;
    bool write_through: 1;
    bool cache_disable: 1;
    bool accessed: 1;
    bool dirty: 1;
    bool memory_type: 1;
    bool global: 1;
    uint8_t _ignored_0: 3;
    size_t page_address: 40;
    uint8_t _ignored_1: 7;
    uint8_t protection_key: 4;
    bool execute_disable: 1;
};

const size_t page_table_length = 512;

const size_t pdp_table_count = 4;
const size_t pd_table_count = 4;
const size_t page_table_count = 16;

struct PageTables {
    PML4Entry pml4_table[page_table_length];

    bool pdp_tables_used[pdp_table_count];
    __attribute__((aligned(page_size)))
    PDPEntry pdp_tables[pdp_table_count][page_table_length];

    bool pd_tables_used[pdp_table_count];
    __attribute__((aligned(page_size)))
    PDEntry pd_tables[pd_table_count][page_table_length];

    bool page_tables_used[pdp_table_count];
    __attribute__((aligned(page_size)))
    PageEntry page_tables[page_table_count][page_table_length];
};

bool map_consecutive_pages(PageTables *tables, size_t physical_pages_start, size_t logical_pages_start, size_t page_count, bool invalidate_pages, bool user_mode);

bool map_pages(PageTables *tables, size_t physical_pages_start, size_t page_count, bool user_mode, size_t *logical_pages_start, bool invalidate_pages);
void unmap_pages(PageTables *tables, size_t logical_pages_start, size_t page_count, bool invalidate_pages);

bool find_unoccupied_physical_pages(
    size_t page_count,
    const PageTables *kernel_tables,
    const Process *processes,
    const MemoryMapEntry *memory_map,
    size_t memory_map_size,
    size_t *physical_pages_start
);

void *map_memory(PageTables *tables, size_t physical_memory_start, size_t size, bool user_mode, bool invalidate_pages);
void unmap_memory(PageTables *tables, void *logical_memory_start, size_t size, bool invalidate_pages);

bool find_unoccupied_physical_memory(
    size_t size,
    const PageTables *kernel_tables,
    const Process *processes,
    const MemoryMapEntry *memory_map,
    size_t memory_map_size,
    size_t *physical_memory_start
);