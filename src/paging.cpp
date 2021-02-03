#include "paging.h"
#include <stdint.h>
#include "process.h"
#include "console.h"

bool map_consecutive_pages(PageTables *tables, size_t physical_pages_start, size_t logical_pages_start, size_t page_count, bool invalidate_pages, bool user_mode) {
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
        if(tables->pml4_table[pml4_index].present) {
            pdp_table = (PDPEntry*)(tables->pml4_table[pml4_index].pdp_table_page_address * page_size);
        } else {
            auto found = false;
            for(size_t i = 0; i < pdp_table_count; i += 1) {
                if(!tables->pdp_tables_used[i]) {
                    pdp_table = tables->pdp_tables[i];
                    tables->pdp_tables_used[i] = true;
                    found = true;

                    break;
                }
            }

            if(!found) {
                return false;
            }

            tables->pml4_table[pml4_index].present = true;
            tables->pml4_table[pml4_index].write_allowed = true;
            tables->pml4_table[pml4_index].user_mode_allowed = true;
            tables->pml4_table[pml4_index].pdp_table_page_address = (size_t)pdp_table / page_size;
        }

        PDEntry *pd_table;
        if(pdp_table[pdp_index].present) {
            pd_table = (PDEntry*)(pdp_table[pdp_index].pd_table_page_address * page_size);
        } else {
            auto found = false;
            for(size_t i = 0; i < pd_table_count; i += 1) {
                if(!tables->pd_tables_used[i]) {
                    pd_table = tables->pd_tables[i];
                    tables->pd_tables_used[i] = true;
                    found = true;

                    break;
                }
            }

            if(!found) {
                return false;
            }


            pdp_table[pdp_index].present = true;
            pdp_table[pdp_index].write_allowed = true;
            pdp_table[pdp_index].user_mode_allowed = true;
            pdp_table[pdp_index].pd_table_page_address = (size_t)pd_table / page_size;
        }

        PageEntry *page_table;
        if(pd_table[pd_index].present) {
            page_table = (PageEntry*)(pd_table[pd_index].page_table_page_address * page_size);
        } else {
            auto found = false;
            for(size_t i = 0; i < page_table_count; i += 1) {
                if(!tables->page_tables_used[i]) {
                    page_table = tables->page_tables[i];
                    tables->page_tables_used[i] = true;
                    found = true;

                    break;
                }
            }

            if(!found) {
                return false;
            }


            pd_table[pd_index].present = true;
            pd_table[pd_index].write_allowed = true;
            pd_table[pd_index].user_mode_allowed = true;
            pd_table[pd_index].page_table_page_address = (size_t)page_table / page_size;
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].user_mode_allowed = user_mode;
        page_table[page_index].page_address = physical_pages_start + relative_page_index;

        if(invalidate_pages) {
            __asm volatile(
                "invlpg (%0)"
                :
                : "D"((logical_pages_start + relative_page_index) * page_size)
            );
        }
    }

    return true;
}

bool map_pages(PageTables *tables, size_t physical_pages_start, size_t page_count, bool user_mode, size_t *logical_pages_start, bool invalidate_pages) {
    auto last_full = true;
    auto found = false;
    size_t free_page_range_start;

    size_t total_page_index = 0;
    for(size_t pml4_index = 0; pml4_index < page_table_length; pml4_index += 1) {
        if(!tables->pml4_table[pml4_index].present) {
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
            auto pdp_table = (PDPEntry*)(tables->pml4_table[pml4_index].pdp_table_page_address * page_size);

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

    if(!found) {
        return false;
    }

    if(!map_consecutive_pages(tables, physical_pages_start, free_page_range_start, page_count, true, user_mode)) {
        return false;
    }

    *logical_pages_start = free_page_range_start;

    return true;
}

void unmap_pages(PageTables *tables, size_t logical_pages_start, size_t page_count, bool invalidate_pages) {
    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto page_index = logical_pages_start + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        auto pdp_table = (PDPEntry*)(tables->pml4_table[pml4_index].pdp_table_page_address * page_size);

        auto pd_table = (PDEntry*)(pdp_table[pdp_index].pd_table_page_address * page_size);

        auto page_table = (PageEntry*)(pd_table[pd_index].page_table_page_address * page_size);

        page_table[page_index].present = false;

        if(pd_table[pd_index].present) {
            auto page_table_empty = true;
            for(size_t i = 0; i < page_table_length; i += 1) {
                if(page_table[i].present) {
                    page_table_empty = false;
                    break;
                }
            }

            if(page_table_empty) {
                pd_table[pd_index].present = false;

                auto page_table_index = ((size_t)page_table - (size_t)tables->page_tables) / (sizeof(PageEntry) * page_table_length);

                tables->page_tables_used[page_table_index] = false;
            }
        }

        if(pdp_table[pdp_index].present) {
            auto pd_table_empty = true;
            for(size_t i = 0; i < page_table_length; i += 1) {
                if(pd_table[i].present) {
                    pd_table_empty = false;
                    break;
                }
            }

            if(pd_table_empty) {
                pdp_table[pdp_index].present = false;

                auto pd_table_index = ((size_t)pd_table - (size_t)tables->pd_tables) / (sizeof(PDEntry) * page_table_length);

                tables->pd_tables_used[pd_table_index] = false;
            }
        }

        if(tables->pml4_table[pml4_index].present) {
            auto pdp_table_empty = true;
            for(size_t i = 0; i < page_table_length; i += 1) {
                if(pdp_table[i].present) {
                    pdp_table_empty = false;
                    break;
                }
            }

            if(pdp_table_empty) {
                tables->pml4_table[pml4_index].present = false;

                auto pdp_table_index = ((size_t)pdp_table - (size_t)tables->pdp_tables) / (sizeof(PDPEntry) * page_table_length);

                tables->pdp_tables_used[pdp_table_index] = false;
            }
        }

        __asm volatile(
            "invlpg (%0)"
            :
            : "D"((logical_pages_start + relative_page_index) * page_size)
        );
    }
}

inline size_t divide_round_up(size_t dividend, size_t divisor) {
    return (dividend + divisor / 2) / divisor;
}

static bool check_for_physical_page(const PageTables *tables, size_t physical_page_index) {
    for(size_t page_table_index = 0; page_table_index < page_table_count; page_table_index += 1) {
        if(tables->page_tables_used[page_table_index]) {
            for(size_t page_index = 0; page_index < page_table_length; page_index += 1) {
                if(tables->page_tables[page_table_index][page_index].present) {
                    if(tables->page_tables[page_table_index][page_index].page_address == physical_page_index) {
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

bool find_unoccupied_physical_pages(
    size_t page_count,
    const PageTables *kernel_tables,
    const Process *processes,
    const MemoryMapEntry *memory_map,
    size_t memory_map_size,
    size_t *physical_pages_start
) {
    auto found = false;
    size_t free_page_range_start;

    for(size_t i = 0; i < memory_map_size; i += 1) {
        if(memory_map[i].available) {
            // Early out for memory regions entirely within the kernel memory
            if((size_t)memory_map[i].address >= kernel_memory_start && (size_t)memory_map[i].address + memory_map[i].length <= kernel_memory_end) {
                continue;
            }

            auto last_unavailable = true;

            for(
                auto physical_page_index = divide_round_up((size_t)memory_map[i].address, page_size);
                (physical_page_index + 1) * page_size <= (size_t)memory_map[i].address + memory_map[i].length;
                physical_page_index += 1
            ) {
                if(physical_page_index * page_size < kernel_memory_end && (physical_page_index + 1) * page_size > kernel_memory_start) {
                    continue;
                }

                auto already_allocated = false;

                already_allocated = check_for_physical_page(kernel_tables, physical_page_index);

                if(!already_allocated) {
                    for(size_t j = 0; j < process_count; j += 1) {
                        already_allocated = check_for_physical_page(&processes[j].tables, physical_page_index);

                        if(already_allocated) {
                            break;
                        }
                    }
                }

                if(already_allocated) {
                    last_unavailable = true;

                    continue;
                } else {
                    if(last_unavailable) {
                        free_page_range_start = physical_page_index;

                        last_unavailable = false;
                    }

                    if(physical_page_index - free_page_range_start == page_count) {
                        found = true;

                        break;
                    }
                }
            }
        }

        if(found) {
            break;
        }
    }

    if(!found) {
        return false;
    }

    *physical_pages_start = free_page_range_start;
    return true;
}

void *map_memory(PageTables *tables, size_t physical_memory_start, size_t size, bool user_mode, bool invalidate_pages) {
    auto physical_pages_start = physical_memory_start / page_size;
    auto physical_pages_end = (physical_memory_start + size) / page_size;

    auto offset = physical_memory_start - physical_pages_start * page_size;

    size_t logical_pages_start;
    if(!map_pages(tables, physical_pages_start, physical_pages_end - physical_pages_start + 1, user_mode, &logical_pages_start, invalidate_pages)) {
        return nullptr;
    }

    return (void*)(logical_pages_start * page_size + offset);
}

void unmap_memory(PageTables *tables, void *logical_memory_start, size_t size, bool invalidate_pages) {
    auto logical_pages_start = (size_t)logical_memory_start / page_size;
    auto logical_pages_end = ((size_t)logical_memory_start + size) / page_size;

    unmap_pages(tables, logical_pages_start, logical_pages_end - logical_pages_start + 1, invalidate_pages);
}

bool find_unoccupied_physical_memory(
    size_t size,
    const PageTables *kernel_tables,
    const Process *processes,
    const MemoryMapEntry *memory_map,
    size_t memory_map_size,
    size_t *physical_memory_start
) {
    auto page_count = divide_round_up(size, page_size);

    size_t physical_pages_start;
    if(!find_unoccupied_physical_pages(page_count, kernel_tables, processes, memory_map, memory_map_size, &physical_pages_start)) {
        return false;
    }

    *physical_memory_start = physical_pages_start * page_size;
    return true;
}