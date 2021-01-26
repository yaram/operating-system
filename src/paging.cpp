#include "paging.h"
#include <stdint.h>

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

void create_initial_pages(size_t identity_pages_start, size_t identity_page_count) {
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

    map_consecutive_pages(identity_pages_start, identity_pages_start, identity_page_count, false);

    __asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&pml4_table)
    );
}

void map_consecutive_pages(size_t physical_pages_start, size_t logical_pages_start, size_t page_count, bool invalidate_pages) {
    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto page_index = logical_pages_start + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        PDPEntry *pdp_table;
        if(pml4_table[pml4_index].present) {
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

            pml4_table[pml4_index].present = true;
            pml4_table[pml4_index].write_allowed = true;
            pml4_table[pml4_index].pdp_table_page_address = (size_t)pdp_table / page_size;
        }

        PDEntry *pd_table;
        if(pdp_table[pdp_index].present) {
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

            pdp_table[pdp_index].present = true;
            pdp_table[pdp_index].write_allowed = true;
            pdp_table[pdp_index].pd_table_page_address = (size_t)pd_table / page_size;
        }

        PageEntry *page_table;
        if(pd_table[pd_index].present) {
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

            pd_table[pd_index].present = true;
            pd_table[pd_index].write_allowed = true;
            pd_table[pd_index].page_table_page_address = (size_t)page_table / page_size;
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = physical_pages_start + relative_page_index;

        if(invalidate_pages) {
            __asm volatile(
                "invlpg (%0)"
                :
                : "D"((logical_pages_start + relative_page_index) * page_size)
            );
        }
    }
}

size_t map_pages(size_t physical_page_index, size_t page_count) {
    auto last_full = true;
    auto found = false;
    size_t free_page_range_start;

    size_t total_page_index = 0;
    for(size_t pml4_index = 0; pml4_index < page_table_length; pml4_index += 1) {
        if(!pml4_table[pml4_index].present) {
            if(last_full) {
                free_page_range_start = total_page_index;

                last_full = false;
            }

            auto next_total_page_index = total_page_index + page_table_length * page_table_length * page_table_length;

            if(next_total_page_index - free_page_range_start >= page_count) {
                found = true;

                break;
            }

            total_page_index = next_total_page_index;
        } else {
            auto pdp_table = (PDPEntry*)(pml4_table[pml4_index].pdp_table_page_address * page_size);

            for(size_t pdp_index = 0; pdp_index < page_table_length; pdp_index += 1) {
                if(!pdp_table[pdp_index].present) {
                    if(last_full) {
                        free_page_range_start = total_page_index;

                        last_full = false;
                    }

                    auto next_total_page_index = total_page_index + page_table_length * page_table_length;

                    if(next_total_page_index - free_page_range_start >= page_count) {
                        found = true;

                        break;
                    }

                    total_page_index = next_total_page_index;
                } else {
                    auto pd_table = (PDEntry*)(pdp_table[pdp_index].pd_table_page_address * page_size);

                    for(size_t pd_index = 0; pd_index < page_table_length; pd_index += 1) {
                        if(!pd_table[pd_index].present) {
                            if(last_full) {
                                free_page_range_start = total_page_index;

                                last_full = false;
                            }

                            auto next_total_page_index = total_page_index + page_table_length;

                            if(next_total_page_index - free_page_range_start >= page_count) {
                                found = true;

                                break;
                            }

                            total_page_index = next_total_page_index;
                        } else {
                            auto page_table = (PageEntry*)(pd_table[pd_index].page_table_page_address * page_size);

                            for(size_t page_index = 0; page_index < page_table_length; page_index += 1) {
                                if(!page_table[page_index].present) {
                                    if(last_full) {
                                        free_page_range_start = total_page_index;

                                        last_full = false;
                                    }

                                    if(total_page_index - free_page_range_start + 1 == page_count) {
                                        found = true;

                                        break;
                                    }
                                } else {
                                    last_full = true;
                                }

                                total_page_index += 1;
                            }
                        }

                        if(found) {
                            break;
                        }
                    }
                }

                if(found) {
                    break;
                }
            }
        }

        if(found) {
            break;
        }
    }

    map_consecutive_pages(physical_page_index, free_page_range_start, page_count, true);

    return free_page_range_start;
}

void unmap_pages(size_t logical_page_index, size_t page_count) {
    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto page_index = logical_page_index + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        auto pdp_table = (PDPEntry*)(pml4_table[pml4_index].pdp_table_page_address * page_size);

        auto pd_table = (PDEntry*)(pdp_table[pdp_index].pd_table_page_address * page_size);

        auto page_table = (PageEntry*)(pd_table[pd_index].page_table_page_address * page_size);

        page_table[page_index].present = false;

        auto page_table_empty = true;
        for(size_t i = 0; i < page_table_length; i += 1) {
            if(page_table[i].present) {
                page_table_empty = false;
                break;
            }
        }

        if(page_table_empty) {
            pd_table[pd_index].present = false;

            auto page_table_index = ((size_t)page_tables - (size_t)page_table) / 8;

            page_tables_used[page_table_index] = false;
        }

        auto pd_table_empty = true;
        for(size_t i = 0; i < page_table_length; i += 1) {
            if(pd_table[i].present) {
                pd_table_empty = false;
                break;
            }
        }

        if(pd_table_empty) {
            pdp_table[pdp_index].present = false;

            auto pd_table_index = ((size_t)pd_tables - (size_t)pd_table) / 8;

            pd_tables_used[pd_table_index] = false;
        }

        auto pdp_table_empty = true;
        for(size_t i = 0; i < page_table_length; i += 1) {
            if(pdp_table[i].present) {
                pdp_table_empty = false;
                break;
            }
        }

        if(pdp_table_empty) {
            pml4_table[pml4_index].present = false;

            auto pdp_table_index = ((size_t)pdp_tables - (size_t)pdp_table) / 8;

            pdp_tables_used[pdp_table_index] = false;
        }

        __asm volatile(
            "invlpg (%0)"
            :
            : "D"((logical_page_index + relative_page_index) * page_size)
        );
    }
}