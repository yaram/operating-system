#pragma once

#include <stddef.h>
#include <stdint.h>

const size_t page_size = 4096;

struct __attribute__((packed)) PageTableEntry {
    bool present: 1;
    bool write_allowed: 1;
    bool user_mode_allowed: 1;
    bool write_through: 1;
    bool cache_disable: 1;
    bool accessed: 1;
    bool dirty: 1;
    bool page_size: 1;
    bool global: 1;
    uint8_t _ignored_0: 3;
    size_t page_address: 40;
    uint8_t _ignored_1: 7;
    uint8_t protection_key: 4;
    bool execute_disable: 1;
};

const size_t page_table_length = 512;

// Calculate addresses for accessing tables through recursive page tables

inline size_t make_address_canonical(size_t address) {
    const auto shift_amount = 64 - 48;

    return (size_t)((intptr_t)(address << shift_amount) >> shift_amount);
}

inline PageTableEntry *get_pml4_table_pointer() {
    return (PageTableEntry*)(make_address_canonical(
        (size_t)0b111111111 << 39 |
        (size_t)0b111111111 << 30 |
        (size_t)0b111111111 << 21 |
        (size_t)0b111111111 << 12
    ));
}

inline PageTableEntry *get_pdp_table_pointer(size_t pml4_index) {
    return (PageTableEntry*)(make_address_canonical(
        (size_t)0b111111111 << 39 |
        (size_t)0b111111111 << 30 |
        (size_t)0b111111111 << 21 |
        pml4_index << 12
    ));
}

inline PageTableEntry *get_pd_table_pointer(size_t pml4_index, size_t pdp_index) {
    return (PageTableEntry*)(make_address_canonical(
        (size_t)0b111111111 << 39 |
        (size_t)0b111111111 << 30 |
        pml4_index << 21 |
        pdp_index << 12
    ));
}

inline PageTableEntry *get_page_table_pointer(size_t pml4_index, size_t pdp_index, size_t pd_index) {
    return (PageTableEntry*)(make_address_canonical(
        (size_t)0b111111111 << 39 |
        pml4_index << 30 |
        pdp_index << 21 |
        pd_index << 12
    ));
}

bool find_free_logical_pages(size_t page_count, size_t *logical_pages_start);

size_t count_page_tables_needed_for_logical_pages(size_t logical_pages_start, size_t page_count);

bool allocate_next_physical_page(
    size_t *bitmap_index,
    size_t *bitmap_sub_bit_index,
    uint8_t *bitmap_entries,
    size_t bitmap_size,
    size_t *physical_page_index
);

// Kernel table-specific functions

bool set_page(
    size_t logical_page_index,
    size_t physical_page_index,
    uint8_t *bitmap_entries,
    size_t bitmap_size
);

bool map_pages(
    size_t physical_pages_start,
    size_t page_count,
    uint8_t *bitmap_entries,
    size_t bitmap_size,
    size_t *logical_pages_start
);

void unmap_pages(
    size_t logical_pages_start,
    size_t page_count
);

bool map_free_pages(
    size_t page_count,
    uint8_t *bitmap_entries,
    size_t bitmap_size,
    size_t *logical_pages_start
);

void *map_memory(
    size_t physical_memory_start,
    size_t size,
    uint8_t *bitmap_entries,
    size_t bitmap_size
);

void unmap_memory(
    void *logical_memory_start,
    size_t size
);

void *map_free_memory(
    size_t size,
    uint8_t *bitmap_entries,
    size_t bitmap_size
);

// non-kernel (process) page table functions

bool find_free_logical_pages(
    size_t page_count,
    size_t pml4_table_physical_address,
    uint8_t *bitmap_entries,
    size_t bitmap_size,
    size_t *logical_pages_start
);

bool set_page(
    size_t logical_page_index,
    size_t physical_page_index,
    size_t pml4_table_physical_address,
    uint8_t *bitmap_entries,
    size_t bitmap_size
);