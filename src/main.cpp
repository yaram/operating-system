#include <stddef.h>
#include <stdint.h>
#include "console.h"

const size_t page_size = 4096;

struct __attribute__((packed)) PML4Entry {
    bool present_bit: 1;
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
    bool present_bit: 1;
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
    bool present_bit: 1;
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
    bool present_bit: 1;
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

const size_t kernel_start = 0x200000;
const size_t kernel_end = 0x800000;

const size_t page_table_length = 512;

PML4Entry __attribute__((aligned(page_size))) pml4_table[page_table_length];

const size_t pdp_table_count = 4;
bool pdp_tables_used[pdp_table_count];
PDPEntry __attribute__((aligned(page_size))) pdp_tables[pdp_table_count][page_table_length];

const size_t pd_table_count = 4;
bool pd_tables_used[pd_table_count];
PDEntry __attribute__((aligned(page_size))) pd_tables[pd_table_count][page_table_length];

const size_t page_table_count = 16;
bool page_tables_used[page_table_count];
PageEntry __attribute__((aligned(page_size))) page_tables[page_table_count][page_table_length];

extern "C" void main() {
    for(size_t i = 0; i < page_table_length; i += 1) {
        pml4_table[0] = {};
    }

    for(size_t i = 0; i < pdp_table_count; i += 1) {
        pdp_tables_used[i] = false;

        for(size_t j = 0; j < page_table_length; j += 1) {
            pdp_tables[i][j] = {};
        }
    }

    for(size_t i = 0; i < pd_table_count; i += 1) {
        pd_tables_used[i] = false;

        for(size_t j = 0; j < page_table_length; j += 1) {
            pd_tables[i][j] = {};
        }
    }

    for(size_t i = 0; i < page_table_count; i += 1) {
        page_tables_used[i] = false;

        for(size_t j = 0; j < page_table_length; j += 1) {
            page_tables[i][j] = {};
        }
    }

    for(size_t total_page_index = kernel_start / page_size; total_page_index < kernel_end / page_size; total_page_index += 1) {
        auto page_index = total_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        PDPEntry *pdp_table;
        if(pml4_table[pml4_index].present_bit) {
            pdp_table = (PDPEntry*)(pml4_table[pml4_index].pdp_table_page_address * page_size);
        } else {
            size_t pdp_table_index;
            for(size_t i = 0; i < pdp_table_count; i += 1) {
                if(!pdp_tables_used[i]) {
                    pdp_table = pdp_tables[i];
                    pdp_tables_used[i] = true;

                    break;
                }
            }

            pml4_table[pml4_index].present_bit = true;
            pml4_table[pml4_index].write_allowed = true;
            pml4_table[pml4_index].pdp_table_page_address = (size_t)pdp_table / page_size;
        }

        PDEntry *pd_table;
        if(pdp_table[pdp_index].present_bit) {
            pd_table = (PDEntry*)(pdp_table[pdp_index].pd_table_page_address * page_size);
        } else {
            size_t pd_table_index;
            for(size_t i = 0; i < pd_table_count; i += 1) {
                if(!pd_tables_used[i]) {
                    pd_table = pd_tables[i];
                    pd_tables_used[i] = true;

                    break;
                }
            }

            pdp_table[pdp_index].present_bit = true;
            pdp_table[pdp_index].write_allowed = true;
            pdp_table[pdp_index].pd_table_page_address = (size_t)pd_table / page_size;
        }

        PageEntry *page_table;
        if(pd_table[pd_index].present_bit) {
            page_table = (PageEntry*)(pd_table[pd_index].page_table_page_address * page_size);
        } else {
            size_t page_table_index;
            for(size_t i = 0; i < page_table_count; i += 1) {
                if(!page_tables_used[i]) {
                    page_table = page_tables[i];
                    page_tables_used[i] = true;

                    break;
                }
            }

            pd_table[pd_index].present_bit = true;
            pd_table[pd_index].write_allowed = true;
            pd_table[pd_index].page_table_page_address = (size_t)page_table / page_size;
        }

        page_table[page_index].present_bit = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = total_page_index;
    }

    __asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&pml4_table)
    );

}