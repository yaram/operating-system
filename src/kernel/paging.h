#pragma once

#include <stddef.h>
#include <stdint.h>
#include "array.h"

#define divide_round_up(dividend, divisor) (((dividend) + (divisor) - 1) / (divisor))

const size_t page_size = 4096;

const size_t kernel_memory_start = 0;
const size_t kernel_memory_end = 0x800000;

const auto kernel_pages_start = kernel_memory_start / page_size;
const auto kernel_pages_end = divide_round_up(kernel_memory_end, page_size);

static inline void invalidate_memory_page(void *address) {
    asm volatile(
        "invlpg (%0)"
        :
        : "r"(address)
    );
}

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

struct ConstPageWalker {
    const PageTableEntry *pml4_table;

    size_t pml4_index;
    const PageTableEntry *pdp_table;

    size_t pdp_index;
    const PageTableEntry *pd_table;

    size_t pd_index;
    const PageTableEntry *page_table;

    size_t page_index;

    size_t absolute_page_index;
};

bool create_page_walker(
    size_t pml4_table_physical_address,
    size_t start_page_index,
    Array<uint8_t> bitmap,
    ConstPageWalker *result_walker,
    bool not_locked = true
);
void unmap_page_walker(const ConstPageWalker *walker, bool not_locked = true);
bool increment_page_walker(ConstPageWalker *walker, Array<uint8_t> bitmap, bool not_locked = true);

struct PageWalker {
    size_t bitmap_index;
    size_t bitmap_sub_bit_index;

    PageTableEntry *pml4_table;

    size_t pml4_index;
    PageTableEntry *pdp_table;

    size_t pdp_index;
    PageTableEntry *pd_table;

    size_t pd_index;
    PageTableEntry *page_table;

    size_t page_index;

    size_t absolute_page_index;
};

bool create_page_walker(
    size_t pml4_table_physical_address,
    size_t start_page_index,
    Array<uint8_t> bitmap,
    PageWalker *result_walker,
    bool not_locked = true
);
void unmap_page_walker(const PageWalker *walker, bool not_locked = true);
bool increment_page_walker(PageWalker *walker, Array<uint8_t> bitmap, bool not_locked = true);

size_t count_page_tables_needed_for_logical_pages(size_t logical_pages_start, size_t page_count, bool lock = true);

bool allocate_next_physical_page(
    size_t *bitmap_index,
    size_t *bitmap_sub_bit_index,
    Array<uint8_t> bitmap,
    size_t *physical_page_index,
    bool lock = true
);

bool allocate_consecutive_physical_pages(
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *physical_pages_start,
    bool lock = true
);

void allocate_bitmap_range(Array<uint8_t> bitmap, size_t start, size_t count, bool lock = true);

void deallocate_bitmap_range(Array<uint8_t> bitmap, size_t start, size_t count, bool lock = true);

// Kernel table-specific functions

bool map_pages(
    size_t physical_pages_start,
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start,
    bool lock = true
);

void unmap_pages(
    size_t logical_pages_start,
    size_t page_count,
    bool lock = true
);

bool map_and_allocate_pages(
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start,
    bool lock = true
);

bool map_and_allocate_consecutive_pages(
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start,
    size_t *physical_pages_start,
    bool lock = true
);

void unmap_and_deallocate_pages(
    size_t logical_pages_start,
    size_t page_count,
    Array<uint8_t> bitmap,
    bool lock = true
);

void *map_memory(
    size_t physical_memory_start,
    size_t size,
    Array<uint8_t> bitmap,
    bool lock = true
);

void unmap_memory(
    void *logical_memory_start,
    size_t size,
    bool lock = true
);

void *map_and_allocate_memory(
    size_t size,
    Array<uint8_t> bitmap,
    bool lock = true
);

void *map_and_allocate_consecutive_memory(
    size_t size,
    Array<uint8_t> bitmap,
    size_t *physical_memory_start,
    bool lock = true
);

void unmap_and_deallocate_memory(
    void *logical_memory_start,
    size_t size,
    Array<uint8_t> bitmap,
    bool lock = true
);

// non-kernel (process/user) page table functions

enum PagePermissions {
    Write = 1 << 0,
    Execute = 1 << 1
};

bool map_pages(
    size_t physical_pages_start,
    size_t page_count,
    PagePermissions permissions,
    size_t pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start,
    bool lock = true
);

bool map_pages_from_kernel(
    size_t kernel_logical_pages_start,
    size_t page_count,
    PagePermissions permissions,
    size_t user_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *user_logical_pages_start,
    bool lock = true
);

bool map_pages_from_user(
    size_t user_logical_pages_start,
    size_t page_count,
    size_t user_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *kernel_logical_pages_start,
    bool lock = true
);

bool map_pages_between_user(
    size_t from_logical_pages_start,
    size_t page_count,
    PagePermissions permissions,
    size_t from_pml4_table_physical_address,
    size_t to_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *to_logical_pages_start,
    bool lock = true
);

bool unmap_pages(
    size_t logical_pages_start,
    size_t page_count,
    size_t pml4_table_physical_address,
    bool deallocate,
    Array<uint8_t> bitmap,
    bool lock = true
);