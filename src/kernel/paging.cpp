#include "paging.h"
#include <stdint.h>
#include "memory.h"
#include "threading_kernel.h"
#include "multiprocessing.h"

extern volatile bool global_all_processors_initialized;

static volatile bool combined_paging_lock = false;

bool create_page_walker(
    size_t pml4_table_physical_address,
    size_t start_page_index,
    Array<uint8_t> bitmap,
    ConstPageWalker *result_walker,
    bool not_locked
) {
    *result_walker = {};
    result_walker->absolute_page_index = start_page_index;

    result_walker->pml4_table = (PageTableEntry*)map_memory(
        pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap,
        not_locked
    );
    if(result_walker->pml4_table == nullptr) {
        return false;
    }

    return true;
}

void unmap_page_walker(const ConstPageWalker *walker, bool not_locked) {
    unmap_memory((void*)walker->pml4_table, sizeof(PageTableEntry[page_table_length]), not_locked);

    if(walker->pdp_table != nullptr) {
        unmap_memory((void*)walker->pdp_table, sizeof(PageTableEntry[page_table_length]), not_locked);
    }

    if(walker->pd_table != nullptr) {
        unmap_memory((void*)walker->pd_table, sizeof(PageTableEntry[page_table_length]), not_locked);
    }

    if(walker->page_table != nullptr) {
        unmap_memory((void*)walker->page_table, sizeof(PageTableEntry[page_table_length]), not_locked);
    }
}

static bool map_table(
    size_t *current_parent_index,
    const PageTableEntry **current_table,
    const PageTableEntry *parent_table,
    size_t parent_index,
    Array<uint8_t> bitmap,
    bool not_locked
) {
    if(*current_table == nullptr || parent_index != *current_parent_index) {
        if(*current_table != nullptr) {
            unmap_memory((void*)*current_table, sizeof(PageTableEntry[page_table_length]), false);

            *current_table = nullptr;
        }

        size_t logical_page_index;
        if(!map_pages(
            parent_table[parent_index].page_address,
            1,
            bitmap,
            &logical_page_index,
            not_locked
        )) {
            return false;
        }

        *current_table = (PageTableEntry*)(logical_page_index * page_size);
        *current_parent_index = parent_index;
    }

    return true;
}

bool increment_page_walker(ConstPageWalker *walker, Array<uint8_t> bitmap, bool not_locked) {
    auto page_index = walker->absolute_page_index;
    auto pd_index = page_index / page_table_length;
    auto pdp_index = pd_index / page_table_length;
    auto pml4_index = pdp_index / page_table_length;

    page_index %= page_table_length;
    pd_index %= page_table_length;
    pdp_index %= page_table_length;
    pml4_index %= page_table_length;

    if(
        !map_table(
            &walker->pml4_index,
            &walker->pdp_table,
            walker->pml4_table,
            pml4_index,
            bitmap,
            not_locked
        ) ||
        !map_table(
            &walker->pdp_index,
            &walker->pd_table,
            walker->pdp_table,
            pdp_index,
            bitmap,
            not_locked
        ) ||
        !map_table(
            &walker->pd_index,
            &walker->page_table,
            walker->pd_table,
            pd_index,
            bitmap,
            not_locked
        )
    ) {
        unmap_page_walker(walker, not_locked);

        return false;
    }

    walker->page_index = page_index;

    walker->absolute_page_index += 1;

    return true;
}

bool create_page_walker(
    size_t pml4_table_physical_address,
    size_t start_page_index,
    Array<uint8_t> bitmap,
    PageWalker *result_walker,
    bool not_locked
) {
    *result_walker = {};
    result_walker->absolute_page_index = start_page_index;

    result_walker->pml4_table = (PageTableEntry*)map_memory(
        pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap,
        not_locked
    );
    if(result_walker->pml4_table == nullptr) {
        return false;
    }

    return true;
}

void unmap_page_walker(const PageWalker *walker, bool not_locked) {
    unmap_memory((void*)walker->pml4_table, sizeof(PageTableEntry[page_table_length]), not_locked);

    if(walker->pdp_table != nullptr) {
        unmap_memory((void*)walker->pdp_table, sizeof(PageTableEntry[page_table_length]), not_locked);
    }

    if(walker->pd_table != nullptr) {
        unmap_memory((void*)walker->pd_table, sizeof(PageTableEntry[page_table_length]), not_locked);
    }

    if(walker->page_table != nullptr) {
        unmap_memory((void*)walker->page_table, sizeof(PageTableEntry[page_table_length]), not_locked);
    }
}

static bool map_and_maybe_allocate_table(
    size_t *current_parent_index,
    PageTableEntry **current_table,
    PageTableEntry *parent_table,
    size_t parent_index,
    Array<uint8_t> bitmap,
    size_t *bitmap_index,
    size_t *bitmap_sub_bit_index,
    bool not_locked
) {
    if(parent_table[parent_index].present) {
        if(*current_table == nullptr || parent_index != *current_parent_index) {
            if(*current_table != nullptr) {
                unmap_memory(*current_table, sizeof(PageTableEntry[page_table_length]), not_locked);

                *current_table = nullptr;
            }

            size_t logical_page_index;
            if(!map_pages(
                parent_table[parent_index].page_address,
                1,
                bitmap,
                &logical_page_index,
                not_locked
            )) {
                return false;
            }

            *current_table = (PageTableEntry*)(logical_page_index * page_size);
            *current_parent_index = parent_index;
        }
    } else {
        if(*current_table != nullptr) {
            unmap_memory(*current_table, sizeof(PageTableEntry[page_table_length]), false);

            *current_table = nullptr;
        }

        size_t physical_page_index;
        if(!allocate_next_physical_page(
            bitmap_index,
            bitmap_sub_bit_index,
            bitmap,
            &physical_page_index,
            not_locked
        )) {
            return false;
        }

        size_t logical_page_index;
        if(!map_pages(
            physical_page_index,
            1,
            bitmap,
            &logical_page_index,
            not_locked
        )) {
            return false;
        }

        *current_table = (PageTableEntry*)(logical_page_index * page_size);
        *current_parent_index = parent_index;

        memset(*current_table, 0, sizeof(PageTableEntry[page_table_length]));

#ifndef OPTIMIZED
        if(parent_table[parent_index].present) {
            printf("FATAL ERROR: Trying to map already mapped page table entry. Entry index is 0x%zX\n", parent_index);

            halt();
        }
#endif

        parent_table[parent_index].present = true;
        parent_table[parent_index].write_allowed = true;
        parent_table[parent_index].user_mode_allowed = true;
        parent_table[parent_index].page_address = physical_page_index;
    }

    return true;
}

bool increment_page_walker(PageWalker *walker, Array<uint8_t> bitmap, bool not_locked) {
    auto page_index = walker->absolute_page_index;
    auto pd_index = page_index / page_table_length;
    auto pdp_index = pd_index / page_table_length;
    auto pml4_index = pdp_index / page_table_length;

    page_index %= page_table_length;
    pd_index %= page_table_length;
    pdp_index %= page_table_length;
    pml4_index %= page_table_length;

    if(
        !map_and_maybe_allocate_table(
            &walker->pml4_index,
            &walker->pdp_table,
            walker->pml4_table,
            pml4_index,
            bitmap,
            &walker->bitmap_index,
            &walker->bitmap_sub_bit_index,
            not_locked
        ) ||
        !map_and_maybe_allocate_table(
            &walker->pdp_index,
            &walker->pd_table,
            walker->pdp_table,
            pdp_index,
            bitmap,
            &walker->bitmap_index,
            &walker->bitmap_sub_bit_index,
            not_locked
        ) ||
        !map_and_maybe_allocate_table(
            &walker->pd_index,
            &walker->page_table,
            walker->pd_table,
            pd_index,
            bitmap,
            &walker->bitmap_index,
            &walker->bitmap_sub_bit_index,
            not_locked
        )
    ) {
        unmap_page_walker(walker, not_locked);

        return false;
    }

    walker->page_index = page_index;

    walker->absolute_page_index += 1;

    return true;
}

static bool find_free_logical_pages(size_t page_count, size_t *logical_pages_start) {
    auto last_full = true;
    auto found = false;
    size_t free_page_range_start;

    auto pml4_table = get_pml4_table_pointer();

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
            auto pdp_table = get_pdp_table_pointer(pml4_index);

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
                    auto pd_table = get_pd_table_pointer(pml4_index, pdp_index);

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
                            auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

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

    *logical_pages_start = free_page_range_start;
    return true;
}

size_t count_page_tables_needed_for_logical_pages(size_t logical_pages_start, size_t page_count, bool lock) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    size_t new_page_table_count = 0;

    auto initial_page_index = logical_pages_start;
    auto initial_pd_index = initial_page_index / page_table_length;
    auto initial_pdp_index = initial_pd_index / page_table_length;
    auto initial_pml4_index = initial_pdp_index / page_table_length;

    initial_page_index %= page_table_length;
    initial_pd_index %= page_table_length;
    initial_pdp_index %= page_table_length;
    initial_pml4_index %= page_table_length;

    size_t absolute_page_index = logical_pages_start;

    for(size_t pml4_index = initial_pml4_index; pml4_index < page_table_length; pml4_index += 1) {
        auto pml4_table = get_pml4_table_pointer();

        auto pdp_exists = pml4_table[pml4_index].present;

        if(!pdp_exists) {
            new_page_table_count += 1;
        }

        size_t start_pdp_index;
        if(pml4_index == initial_pml4_index) {
            start_pdp_index = initial_pdp_index;
        } else {
            start_pdp_index = 0;
        }

        for(size_t pdp_index = start_pdp_index; pdp_index < page_table_length; pdp_index += 1) {
            bool pd_exists;
            if(pdp_exists) {
                auto pdp_table = get_pdp_table_pointer(pml4_index);

                pd_exists = pdp_table[pdp_index].present;
            } else {
                pd_exists = false;
            }

            if(!pd_exists) {
                new_page_table_count += 1;
            }

            size_t start_pd_index;
            if(pdp_index == initial_pdp_index) {
                start_pd_index = initial_pd_index;
            } else {
                start_pd_index = 0;
            }

            for(size_t pd_index = start_pd_index; pd_index < page_table_length; pd_index += 1) {
                bool page_table_exists;
                if(pd_exists) {
                    auto pd_table = get_pd_table_pointer(pml4_index, pdp_index);

                    page_table_exists = pd_table[pd_index].present;
                } else {
                    page_table_exists = false;
                }

                if(!page_table_exists) {
                    new_page_table_count += 1;
                }

                size_t start_page_index;
                if(pd_index == initial_pd_index) {
                    start_page_index = initial_page_index;
                } else {
                    start_page_index = 0;
                }

                for(size_t page_index = start_page_index; page_index < page_table_length; page_index += 1) {
                    absolute_page_index += 1;

                    if(absolute_page_index == logical_pages_start + page_count) {
                        break;
                    }
                }

                if(absolute_page_index == logical_pages_start + page_count) {
                    break;
                }
            }

            if(absolute_page_index == logical_pages_start + page_count) {
                break;
            }
        }

        if(absolute_page_index == logical_pages_start + page_count) {
            break;
        }
    }

    if(lock) {
        combined_paging_lock = false;
    }

    return new_page_table_count;
}

bool allocate_next_physical_page(
    size_t *bitmap_index,
    size_t *bitmap_sub_bit_index,
    Array<uint8_t> bitmap,
    size_t *physical_page_index,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    auto byte = &bitmap[*bitmap_index];

    if(*byte != 0xFF) {
        for(; *bitmap_sub_bit_index < 8; *bitmap_sub_bit_index += 1) {
            if((*byte & (1 << *bitmap_sub_bit_index)) == 0) {
                *byte |= 1 << *bitmap_sub_bit_index;

                if(lock) {
                    combined_paging_lock = false;
                }

                *physical_page_index = *bitmap_index * 8 + *bitmap_sub_bit_index;
                return true;
            }
        }
    }

    *bitmap_index += 1;

    for(; *bitmap_index < bitmap.length; *bitmap_index += 1) {
        auto byte = &bitmap[*bitmap_index];

        if(*byte != 0xFF) {
            for(*bitmap_sub_bit_index = 0; *bitmap_sub_bit_index < 8; *bitmap_sub_bit_index += 1) {
                if((*byte & (1 << *bitmap_sub_bit_index)) == 0) {
                    *byte |= 1 << *bitmap_sub_bit_index;

                    if(lock) {
                        combined_paging_lock = false;
                    }

                    *physical_page_index = *bitmap_index * 8 + *bitmap_sub_bit_index;
                    return true;
                }
            }
        }
    }

    if(lock) {
        combined_paging_lock = false;
    }

    return false;
}

bool allocate_consecutive_physical_pages(
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *physical_pages_start,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    size_t free_pages_start;
    auto in_free_pages = false;
    auto found = false;

    for(size_t bitmap_index = 0; bitmap_index < bitmap.length; bitmap_index += 1) {
        auto byte = &bitmap[bitmap_index];

        if(*byte != 0xFF) {
            for(size_t bitmap_sub_bit_index = 0; bitmap_sub_bit_index < 8; bitmap_sub_bit_index += 1) {
                auto page_index = bitmap_index * 8 + bitmap_sub_bit_index;

                if(in_free_pages && page_index - free_pages_start == page_count) {
                    found = true;
                    break;
                }

                if((*byte & (1 << bitmap_sub_bit_index)) == 0) {
                    if(!in_free_pages) {
                        free_pages_start = page_index;
                        in_free_pages = true;
                    }
                } else {
                    in_free_pages = false;
                }
            }
        }

        if(found) {
            break;
        }
    }

    if(!found) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    allocate_bitmap_range(bitmap, free_pages_start, page_count, false);

    if(lock) {
        combined_paging_lock = false;
    }

    *physical_pages_start = free_pages_start;
    return true;
}

void allocate_bitmap_range(Array<uint8_t> bitmap, size_t start, size_t count, bool lock) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    auto start_bit = start;
    auto end_bit = start + count;

    auto start_byte = start_bit / 8;
    auto end_byte = divide_round_up(end_bit, 8);

    auto sub_start_bit = start_bit % 8;
    auto sub_end_bit = end_bit % 8;

    if(sub_end_bit == 0) {
        sub_end_bit = 8;
    }

    if(end_byte - start_byte == 1) {
        for(size_t i = sub_start_bit; i < sub_end_bit; i += 1) {
            bitmap[start_byte] |= 1 << i;
        }
    } else {
        for(size_t i = sub_start_bit; i < 8; i += 1) {
            bitmap[start_byte] |= 1 << i;
        }

        for(size_t i = start_byte + 1; i < end_byte - 1; i += 1) {
            bitmap[i] = 0b11111111;
        }

        for(size_t i = 0; i < sub_end_bit; i += 1) {
            bitmap[end_byte - 1] |= 1 << i;
        }
    }

    if(lock) {
        combined_paging_lock = false;
    }
}

void deallocate_bitmap_range(Array<uint8_t> bitmap, size_t start, size_t count, bool lock) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    auto start_bit = start;
    auto end_bit = start + count;

    auto start_byte = start_bit / 8;
    auto end_byte = divide_round_up(end_bit, 8);

    auto sub_start_bit = start_bit % 8;
    auto sub_end_bit = end_bit % 8;

    if(sub_end_bit == 0) {
        sub_end_bit = 8;
    }

    if(end_byte - start_byte == 1) {
        for(size_t i = sub_start_bit; i < sub_end_bit; i += 1) {
            bitmap[start_byte] &= ~(1 << i);
        }
    } else {
        for(size_t i = sub_start_bit; i < 8; i += 1) {
            bitmap[start_byte] &= ~(1 << i);
        }

        for(size_t i = start_byte + 1; i < end_byte - 1; i += 1) {
            bitmap[i] = 0;
        }

        for(size_t i = 0; i < sub_end_bit; i += 1) {
            bitmap[end_byte - 1] &= ~(1 << i);
        }
    }

    if(lock) {
        combined_paging_lock = false;
    }
}

static bool maybe_allocate_kernel_tables(
    size_t pml4_index,
    size_t pdp_index,
    size_t pd_index,
    size_t page_index,
    Array<uint8_t> bitmap
) {
    size_t bitmap_index = 0;
    size_t bitmap_sub_bit_index = 0;

    auto pml4_table = get_pml4_table_pointer();

    auto pdp_table = get_pdp_table_pointer(pml4_index);

    if(!pml4_table[pml4_index].present) {
        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap,
            &physical_page_index,
            false
        )) {
            return false;
        }

#ifndef OPTIMIZED
        if(pml4_table[pml4_index].present) {
            printf("FATAL ERROR: Trying to map already mapped pml4 table entry. Entry index is 0x%zX\n", pml4_index);

            halt();
        }
#endif

        pml4_table[pml4_index].present = true;
        pml4_table[pml4_index].write_allowed = true;
        pml4_table[pml4_index].user_mode_allowed = true;
        pml4_table[pml4_index].page_address = physical_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"(pdp_table)
        );

        memset((void*)pdp_table, 0, sizeof(PageTableEntry[page_table_length]));

        if(global_all_processors_initialized) {
            send_kernel_page_tables_update((size_t)pdp_table / page_size, 1);
        }
    }

    auto pd_table = get_pd_table_pointer(pml4_index, pdp_index);

    if(!pdp_table[pdp_index].present) {
        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap,
            &physical_page_index,
            false
        )) {
            return false;
        }

#ifndef OPTIMIZED
        if(pdp_table[pdp_index].present) {
            printf("FATAL ERROR: Trying to map already mapped pdp table entry. Entry index is 0x%zX\n", pdp_index);

            halt();
        }
#endif

        pdp_table[pdp_index].present = true;
        pdp_table[pdp_index].write_allowed = true;
        pdp_table[pdp_index].user_mode_allowed = true;
        pdp_table[pdp_index].page_address = physical_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"(pd_table)
        );

        memset((void*)pd_table, 0, sizeof(PageTableEntry[page_table_length]));

        if(global_all_processors_initialized) {
            send_kernel_page_tables_update((size_t)pd_table / page_size, 1);
        }
    }

    auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

    if(!pd_table[pd_index].present) {
        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap,
            &physical_page_index,
            false
        )) {
            return false;
        }

#ifndef OPTIMIZED
        if(pd_table[pd_index].present) {
            printf("FATAL ERROR: Trying to map already mapped pd table entry. Entry index is 0x%zX\n", pd_index);

            halt();
        }
#endif

        pd_table[pd_index].present = true;
        pd_table[pd_index].write_allowed = true;
        pd_table[pd_index].user_mode_allowed = true;
        pd_table[pd_index].page_address = physical_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"(page_table)
        );

        memset((void*)page_table, 0, sizeof(PageTableEntry[page_table_length]));

        if(global_all_processors_initialized) {
            send_kernel_page_tables_update((size_t)page_table / page_size, 1);
        }
    }

    return true;
}

bool map_pages(
    size_t physical_pages_start,
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    if(!find_free_logical_pages(page_count, logical_pages_start)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto page_index = *logical_pages_start + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        if(!maybe_allocate_kernel_tables(pml4_index, pdp_index, pd_index, page_index, bitmap)) {
            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

#ifndef OPTIMIZED
        if(page_table[page_index].present) {
            printf("FATAL ERROR: Trying to map already mapped page. Page index is 0x%zX\n", *logical_pages_start + relative_page_index);

            halt();
        }
#endif

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = physical_pages_start + relative_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((*logical_pages_start + relative_page_index) * page_size)
        );
    }

    if(lock) {
        combined_paging_lock = false;
    }

    return true;
}

void unmap_pages(
    size_t logical_pages_start,
    size_t page_count,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto page_index = logical_pages_start + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

#ifndef OPTIMIZED
        if(!page_table[page_index].present) {
            printf("FATAL ERROR: Trying to unmap already unmapped page. Page index is 0x%zX\n", logical_pages_start + relative_page_index);

            halt();
        }
#endif

#ifdef NO_PAGE_REUSE
        page_table[page_index].page_address = 0x7FFFFFFFFF; // Force a page fault on access
#else
        page_table[page_index].present = false;
#endif

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((logical_pages_start + relative_page_index) * page_size)
        );
    }

    if(lock) {
        combined_paging_lock = false;
    }
}

bool map_and_allocate_pages(
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    if(!find_free_logical_pages(page_count, logical_pages_start)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    size_t bitmap_index = 0;
    size_t bitmap_sub_bit_index = 0;

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto page_index = *logical_pages_start + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        if(!maybe_allocate_kernel_tables(pml4_index, pdp_index, pd_index, page_index, bitmap)) {
            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap,
            &physical_page_index,
            false
        )) {
            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

#ifndef OPTIMIZED
        if(page_table[page_index].present) {
            printf("FATAL ERROR: Trying to map already mapped page. Page index is 0x%zX\n", *logical_pages_start + relative_page_index);

            halt();
        }
#endif

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = physical_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((*logical_pages_start + relative_page_index) * page_size)
        );
    }

    if(lock) {
        combined_paging_lock = false;
    }

    return true;
}

bool map_and_allocate_consecutive_pages(
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start,
    size_t *physical_pages_start,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    if(!allocate_consecutive_physical_pages(
        page_count,
        bitmap,
        physical_pages_start,
        false
    )) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    if(!map_pages(
        *physical_pages_start,
        page_count,
        bitmap,
        logical_pages_start,
        false
    )) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    if(lock) {
        combined_paging_lock = false;
    }

    return true;
}

void unmap_and_deallocate_pages(
    size_t logical_pages_start,
    size_t page_count,
    Array<uint8_t> bitmap,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto page_index = logical_pages_start + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

#ifndef OPTIMIZED
        if(!page_table[page_index].present) {
            printf("FATAL ERROR: Trying to unmap already unmapped page. Page index is 0x%zX\n", logical_pages_start + relative_page_index);

            halt();
        }
#endif

#ifdef NO_PAGE_REUSE
        page_table[page_index].page_address = 0x7FFFFFFFFF; // Force a page fault on access
#else
        auto page_address = page_table[page_index].page_address;

        auto bitmap_index = page_address / 8;
        auto bitmap_sub_bit_index = page_address % 8;

        bitmap[bitmap_index] &= ~(1 << bitmap_sub_bit_index);

        page_table[page_index].present = false;
#endif

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((logical_pages_start + relative_page_index) * page_size)
        );
    }

    if(lock) {
        combined_paging_lock = false;
    }
}

void *map_memory(
    size_t physical_memory_start,
    size_t size,
    Array<uint8_t> bitmap,
    bool lock
) {
    auto physical_pages_start = physical_memory_start / page_size;
    auto physical_pages_end = divide_round_up(physical_memory_start + size, page_size);

    auto page_count = physical_pages_end - physical_pages_start;

    auto offset = physical_memory_start - physical_pages_start * page_size;

    size_t logical_pages_start;
    if(!map_pages(physical_pages_start, page_count, bitmap, &logical_pages_start, lock)) {
        return nullptr;
    }

    return (void*)(logical_pages_start * page_size + offset);
}

void unmap_memory(
    void *logical_memory_start,
    size_t size,
    bool lock
) {
    auto logical_pages_start = (size_t)logical_memory_start / page_size;
    auto logical_pages_end = divide_round_up((size_t)logical_memory_start + size, page_size);

    auto page_count = logical_pages_end - logical_pages_start;

    return unmap_pages(logical_pages_start, page_count, lock);
}

void *map_and_allocate_memory(
    size_t size,
    Array<uint8_t> bitmap,
    bool lock
) {
    auto page_count = divide_round_up(size, page_size);

    size_t logical_pages_start;
    if(!map_and_allocate_pages(
        page_count,
        bitmap,
        &logical_pages_start,
        lock
    )) {
        return nullptr;
    }

    return (void*)(logical_pages_start * page_size);
}

void *map_and_allocate_consecutive_memory(
    size_t size,
    Array<uint8_t> bitmap,
    size_t *physical_memory_start,
    bool lock
) {
    auto page_count = divide_round_up(size, page_size);

    size_t logical_pages_start;
    size_t physical_pages_start;
    if(!map_and_allocate_consecutive_pages(
        page_count,
        bitmap,
        &logical_pages_start,
        &physical_pages_start,
        lock
    )) {
        return nullptr;
    }

    *physical_memory_start = physical_pages_start * page_size;
    return (void*)(logical_pages_start * page_size);
}

void unmap_and_deallocate_memory(
    void *logical_memory_start,
    size_t size,
    Array<uint8_t> bitmap,
    bool lock
) {
    auto logical_pages_start = (size_t)logical_memory_start / page_size;
    auto logical_pages_end = divide_round_up((size_t)logical_memory_start + size, page_size);

    auto page_count = logical_pages_end - logical_pages_start;

    unmap_and_deallocate_pages(logical_pages_start, page_count, bitmap, lock);
}

static bool find_free_logical_pages(
    size_t page_count,
    size_t pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start
) {
    auto last_full = true;
    auto found = false;
    size_t free_page_range_start;

    auto pml4_table = (PageTableEntry*)map_memory(
        pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap,
        false
    );
    if(pml4_table == nullptr) {
        return false;
    }

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
            auto pdp_table = (PageTableEntry*)map_memory(
                pml4_table[pml4_index].page_address * page_size,
                sizeof(PageTableEntry[page_table_length]),
                bitmap,
                false
            );
            if(pdp_table == nullptr) {
                unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]), false);

                return false;
            }

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
                    auto pd_table = (PageTableEntry*)map_memory(
                        pdp_table[pdp_index].page_address * page_size,
                        sizeof(PageTableEntry[page_table_length]),
                        bitmap,
                        false
                    );
                    if(pd_table == nullptr) {
                        unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]), false);
                        unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]), false);

                        return false;
                    }

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
                            auto page_table = (PageTableEntry*)map_memory(
                                pd_table[pd_index].page_address * page_size,
                                sizeof(PageTableEntry[page_table_length]),
                                bitmap,
                                false
                            );
                            if(page_table == nullptr) {
                                unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]), false);
                                unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]), false);
                                unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]), false);

                                return false;
                            }

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

                            unmap_memory(page_table, sizeof(PageTableEntry[page_table_length]), false);
                        }

                        if(found) {
                            break;
                        }
                    }

                    unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]), false);
                }

                if(found) {
                    break;
                }
            }

            unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]), false);
        }

        if(found) {
            break;
        }
    }

    unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]), false);

    if(!found) {
        return false;
    }

    *logical_pages_start = free_page_range_start;
    return true;
}

bool map_pages(
    size_t physical_pages_start,
    size_t page_count,
    PagePermissions permissions,
    size_t pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    if(!find_free_logical_pages(page_count, pml4_table_physical_address, bitmap, logical_pages_start)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    PageWalker walker;
    if(!create_page_walker(pml4_table_physical_address, *logical_pages_start, bitmap, &walker, !lock)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    for(
        size_t relative_page_index = 0;
        relative_page_index < page_count;
        relative_page_index += 1
    ) {
        if(!increment_page_walker(&walker, bitmap, !lock)) {
            unmap_page_walker(&walker, !lock);

            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

        auto page = &walker.page_table[walker.page_index];

#ifndef OPTIMIZED
        if(page->present) {
            printf("FATAL ERROR: Trying to map already mapped page. Page index is 0x%zX\n", walker.absolute_page_index);

            halt();
        }
#endif

        page->present = true;
        page->write_allowed = permissions & PagePermissions::Write;
        page->execute_disable = !(permissions & PagePermissions::Execute);
        page->user_mode_allowed = true;
        page->page_address = physical_pages_start + relative_page_index;
    }

    unmap_page_walker(&walker, !lock);

    if(lock) {
        combined_paging_lock = false;
    }

    return true;
}

bool map_pages_from_kernel(
    size_t kernel_logical_pages_start,
    size_t page_count,
    PagePermissions permissions,
    size_t user_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *user_logical_pages_start,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    if(!find_free_logical_pages(page_count, user_pml4_table_physical_address, bitmap, user_logical_pages_start)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    PageWalker walker;
    if(!create_page_walker(user_pml4_table_physical_address, *user_logical_pages_start, bitmap, &walker, !lock)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto kernel_page_index = kernel_logical_pages_start + relative_page_index;
        auto kernel_pd_index = kernel_page_index / page_table_length;
        auto kernel_pdp_index = kernel_pd_index / page_table_length;
        auto kernel_pml4_index = kernel_pdp_index / page_table_length;

        kernel_page_index %= page_table_length;
        kernel_pd_index %= page_table_length;
        kernel_pdp_index %= page_table_length;
        kernel_pml4_index %= page_table_length;

        if(!increment_page_walker(&walker, bitmap, !lock)) {
            unmap_page_walker(&walker, !lock);

            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

        auto kernel_page_table = get_page_table_pointer(kernel_pml4_index, kernel_pdp_index, kernel_pd_index);

        auto user_page = &walker.page_table[walker.page_index];

#ifndef OPTIMIZED
        if(user_page->present) {
            printf("FATAL ERROR: Trying to map already mapped page. Page index is 0x%zX\n", walker.absolute_page_index);

            halt();
        }

        if(!kernel_page_table[kernel_page_index].present) {
            printf("FATAL ERROR: Trying to reference unmapped page. Page index is 0x%zX\n", kernel_logical_pages_start + relative_page_index);

            halt();
        }
#endif

        user_page->present = true;
        user_page->write_allowed = permissions & PagePermissions::Write;
        user_page->execute_disable = !(permissions & PagePermissions::Execute);
        user_page->user_mode_allowed = true;
        user_page->page_address = kernel_page_table[kernel_page_index].page_address;
    }

    unmap_page_walker(&walker, !lock);

    if(lock) {
        combined_paging_lock = false;
    }

    return true;
}

bool map_pages_from_user(
    size_t user_logical_pages_start,
    size_t page_count,
    size_t user_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *kernel_logical_pages_start,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    if(!find_free_logical_pages(page_count, kernel_logical_pages_start)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    // Pre-allocate kernel pages so they won't be overwritten by subsequent map_table calls below
    for(
        size_t absolute_page_index = *kernel_logical_pages_start;
        absolute_page_index < *kernel_logical_pages_start + page_count;
        absolute_page_index += 1
    ) {
        auto page_index = absolute_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        if(!maybe_allocate_kernel_tables(pml4_index, pdp_index, pd_index, page_index, bitmap)) {
            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

#ifndef OPTIMIZED
        if(page_table[page_index].present) {
            printf("FATAL ERROR: Trying to map already mapped page. Page index is 0x%zX\n", absolute_page_index);

            halt();
        }
#endif

        page_table[page_index].present = true;
    }

    ConstPageWalker walker;
    if(!create_page_walker(user_pml4_table_physical_address, user_logical_pages_start, bitmap, &walker, !lock)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto kernel_page_index = *kernel_logical_pages_start + relative_page_index;
        auto kernel_pd_index = kernel_page_index / page_table_length;
        auto kernel_pdp_index = kernel_pd_index / page_table_length;
        auto kernel_pml4_index = kernel_pdp_index / page_table_length;

        kernel_page_index %= page_table_length;
        kernel_pd_index %= page_table_length;
        kernel_pdp_index %= page_table_length;
        kernel_pml4_index %= page_table_length;

        if(!increment_page_walker(&walker, bitmap, !lock)) {
            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

        auto kernel_page_table = get_page_table_pointer(kernel_pml4_index, kernel_pdp_index, kernel_pd_index);

#ifndef OPTIMIZED
        if(!walker.page_table[walker.page_index].present) {
            printf("FATAL ERROR: Trying to reference unmapped page. Page index is 0x%zX\n", walker.absolute_page_index);

            halt();
        }
#endif

        kernel_page_table[kernel_page_index].write_allowed = true;
        kernel_page_table[kernel_page_index].user_mode_allowed = false;
        kernel_page_table[kernel_page_index].page_address = walker.page_table[walker.page_index].page_address;
    }

    unmap_page_walker(&walker, !lock);

    if(lock) {
        combined_paging_lock = false;
    }

    return true;
}

bool map_pages_between_user(
    size_t from_logical_pages_start,
    size_t page_count,
    PagePermissions permissions,
    size_t from_pml4_table_physical_address,
    size_t to_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *to_logical_pages_start,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    if(!find_free_logical_pages(page_count, to_pml4_table_physical_address, bitmap, to_logical_pages_start)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    ConstPageWalker from_walker;
    if(!create_page_walker(from_pml4_table_physical_address, from_logical_pages_start, bitmap, &from_walker, !lock)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    PageWalker to_walker;
    if(!create_page_walker(to_pml4_table_physical_address, *to_logical_pages_start, bitmap, &to_walker, !lock)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        if(!increment_page_walker(&from_walker, bitmap, !lock) || !increment_page_walker(&to_walker, bitmap, !lock)) {
            unmap_page_walker(&from_walker, !lock);
            unmap_page_walker(&to_walker, !lock);

            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

        auto from_page = &from_walker.page_table[from_walker.page_index];
        auto to_page = &to_walker.page_table[to_walker.page_index];

#ifndef OPTIMIZED
        if(to_page->present) {
            printf("FATAL ERROR: Trying to map already mapped page. Page index is 0x%zX\n", to_walker.absolute_page_index);

            halt();
        }

        if(!from_page->present) {
            printf("FATAL ERROR: Trying to reference unmapped page. Page index is 0x%zX\n", from_walker.absolute_page_index);

            halt();
        }
#endif

        to_page->present = true;
        to_page->write_allowed = permissions & PagePermissions::Write;
        to_page->execute_disable = !(permissions & PagePermissions::Execute);
        to_page->user_mode_allowed = true;
        to_page->page_address = from_page->page_address;
    }

    unmap_page_walker(&from_walker, !lock);
    unmap_page_walker(&to_walker, !lock);

    if(lock) {
        combined_paging_lock = false;
    }

    return true;
}

bool unmap_pages(
    size_t logical_pages_start,
    size_t page_count,
    size_t pml4_table_physical_address,
    bool deallocate,
    Array<uint8_t> bitmap,
    bool lock
) {
    if(lock) {
        acquire_lock(&combined_paging_lock);
    }

    PageWalker walker;
    if(!create_page_walker(pml4_table_physical_address, logical_pages_start, bitmap, &walker, !lock)) {
        if(lock) {
            combined_paging_lock = false;
        }

        return false;
    }

    for(
        size_t absolute_page_index = logical_pages_start;
        absolute_page_index < logical_pages_start + page_count;
        absolute_page_index += 1
    ) {
        if(!increment_page_walker(&walker, bitmap, !lock)) {
            unmap_page_walker(&walker, !lock);

            if(lock) {
                combined_paging_lock = false;
            }

            return false;
        }

        auto page = &walker.page_table[walker.page_index];

#ifndef OPTIMIZED
        if(!page->present) {
            printf("FATAL ERROR: Trying to unmap already unmapped page. Page index is 0x%zX\n", walker.absolute_page_index);

            halt();
        }
#endif

#ifdef NO_PAGE_REUSE
        page->page_address = 0x7FFFFFFFFF; // Force a page fault on access
#else
        page->present = false;

        if(deallocate) {
            auto physical_page_index = page->page_address;

            auto byte_index = physical_page_index / 8;
            auto sub_byte_index = physical_page_index % 8;

            bitmap[byte_index] &= ~(1 << sub_byte_index);
        }
#endif
    }

    unmap_page_walker(&walker, !lock);

    if(lock) {
        combined_paging_lock = false;
    }

    return true;
}