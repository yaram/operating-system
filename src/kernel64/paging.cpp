#include "paging.h"
#include <stdint.h>
#include "memory.h"

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

size_t count_page_tables_needed_for_logical_pages(size_t logical_pages_start, size_t page_count) {
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

    return new_page_table_count;
}

bool allocate_next_physical_page(
    size_t *bitmap_index,
    size_t *bitmap_sub_bit_index,
    Array<uint8_t> bitmap,
    size_t *physical_page_index
) {
    auto byte = &bitmap[*bitmap_index];

    if(*byte != 0xFF) {
        for(; *bitmap_sub_bit_index < 8; *bitmap_sub_bit_index += 1) {
            if((*byte & (1 << *bitmap_sub_bit_index)) == 0) {
                *byte |= 1 << *bitmap_sub_bit_index;

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

                    *physical_page_index = *bitmap_index * 8 + *bitmap_sub_bit_index;
                    return true;
                }
            }
        }
    }

    return false;
}

bool allocate_consecutive_physical_pages(
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *physical_pages_start
) {
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
        return false;
    }

    allocate_bitmap_range(bitmap, free_pages_start, page_count);

    *physical_pages_start = free_pages_start;
    return true;
}

void allocate_bitmap_range(Array<uint8_t> bitmap, size_t start, size_t count) {
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
}

void deallocate_bitmap_range(Array<uint8_t> bitmap, size_t start, size_t count) {
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
}

bool map_pages(
    size_t physical_pages_start,
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start
) {
    if(!find_free_logical_pages(page_count, logical_pages_start)) {
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

        auto pml4_table = get_pml4_table_pointer();

        auto pdp_table = get_pdp_table_pointer(pml4_index);

        if(!pml4_table[pml4_index].present) {
            size_t physical_page_index;
            if(!allocate_next_physical_page(
                &bitmap_index,
                &bitmap_sub_bit_index,
                bitmap,
                &physical_page_index
            )) {
                return false;
            }

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
        }

        auto pd_table = get_pd_table_pointer(pml4_index, pdp_index);

        if(!pdp_table[pdp_index].present) {
            size_t physical_page_index;
            if(!allocate_next_physical_page(
                &bitmap_index,
                &bitmap_sub_bit_index,
                bitmap,
                &physical_page_index
            )) {
                return false;
            }

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
        }

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

        if(!pd_table[pd_index].present) {
            size_t physical_page_index;
            if(!allocate_next_physical_page(
                &bitmap_index,
                &bitmap_sub_bit_index,
                bitmap,
                &physical_page_index
            )) {
                return false;
            }

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
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = physical_pages_start + relative_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((*logical_pages_start + relative_page_index) * page_size)
        );
    }

    return true;
}

void unmap_pages(
    size_t logical_pages_start,
    size_t page_count
) {
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

        page_table[page_index].present = false;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((logical_pages_start + relative_page_index) * page_size)
        );
    }
}

bool map_and_allocate_pages(
    size_t page_count,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start
) {
    if(!find_free_logical_pages(page_count, logical_pages_start)) {
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

        auto pml4_table = get_pml4_table_pointer();

        auto pdp_table = get_pdp_table_pointer(pml4_index);

        if(!pml4_table[pml4_index].present) {
            size_t physical_page_index;
            if(!allocate_next_physical_page(
                &bitmap_index,
                &bitmap_sub_bit_index,
                bitmap,
                &physical_page_index
            )) {
                return false;
            }

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
        }

        auto pd_table = get_pd_table_pointer(pml4_index, pdp_index);

        if(!pdp_table[pdp_index].present) {
            size_t physical_page_index;
            if(!allocate_next_physical_page(
                &bitmap_index,
                &bitmap_sub_bit_index,
                bitmap,
                &physical_page_index
            )) {
                return false;
            }

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
        }

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

        if(!pd_table[pd_index].present) {
            size_t physical_page_index;
            if(!allocate_next_physical_page(
                &bitmap_index,
                &bitmap_sub_bit_index,
                bitmap,
                &physical_page_index
            )) {
                return false;
            }

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
        }

        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap,
            &physical_page_index
        )) {
            return false;
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = physical_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((*logical_pages_start + relative_page_index) * page_size)
        );
    }

    return true;
}

void unmap_and_deallocate_pages(
    size_t logical_pages_start,
    size_t page_count,
    Array<uint8_t> bitmap
) {
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

        auto page_address = page_table[page_index].page_address;

        auto bitmap_index = page_address / 8;
        auto bitmap_sub_bit_index = page_address % 8;

        bitmap[bitmap_index] &= ~(1 << bitmap_sub_bit_index);

        page_table[page_index].present = false;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((logical_pages_start + relative_page_index) * page_size)
        );
    }
}

void *map_memory(
    size_t physical_memory_start,
    size_t size,
    Array<uint8_t> bitmap
) {
    auto physical_pages_start = physical_memory_start / page_size;
    auto physical_pages_end = divide_round_up(physical_memory_start + size, page_size);

    auto page_count = physical_pages_end - physical_pages_start;

    auto offset = physical_memory_start - physical_pages_start * page_size;

    size_t logical_pages_start;
    if(!map_pages(physical_pages_start, page_count, bitmap, &logical_pages_start)) {
        return nullptr;
    }

    return (void*)(logical_pages_start * page_size + offset);
}

void unmap_memory(
    void *logical_memory_start,
    size_t size
) {
    auto logical_pages_start = (size_t)logical_memory_start / page_size;
    auto logical_pages_end = divide_round_up((size_t)logical_memory_start + size, page_size);

    auto page_count = logical_pages_end - logical_pages_start;

    return unmap_pages(logical_pages_start, page_count);
}

void *map_and_allocate_memory(
    size_t size,
    Array<uint8_t> bitmap
) {
    auto page_count = divide_round_up(size, page_size);

    size_t logical_pages_start;
    if(!map_and_allocate_pages(
        page_count,
        bitmap,
        &logical_pages_start
    )) {
        return nullptr;
    }

    return (void*)(logical_pages_start * page_size);
}

void unmap_and_deallocate_memory(
    void *logical_memory_start,
    size_t size,
    Array<uint8_t> bitmap
) {
    auto logical_pages_start = (size_t)logical_memory_start / page_size;
    auto logical_pages_end = divide_round_up((size_t)logical_memory_start + size, page_size);

    auto page_count = logical_pages_end - logical_pages_start;

    unmap_and_deallocate_pages(logical_pages_start, page_count, bitmap);
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
        bitmap
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
                bitmap
            );
            if(pdp_table == nullptr) {
                unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));

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
                        bitmap
                    );
                    if(pd_table == nullptr) {
                        unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));
                        unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));

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
                                bitmap
                            );
                            if(page_table == nullptr) {
                                unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));
                                unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
                                unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));

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

                            unmap_memory(page_table, sizeof(PageTableEntry[page_table_length]));
                        }

                        if(found) {
                            break;
                        }
                    }

                    unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));
                }

                if(found) {
                    break;
                }
            }

            unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
        }

        if(found) {
            break;
        }
    }

    unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));

    if(!found) {
        return false;
    }

    *logical_pages_start = free_page_range_start;
    return true;
}

static bool map_and_maybe_allocate_table(
    size_t *current_parent_index,
    PageTableEntry **current_table,
    PageTableEntry *parent_table,
    size_t parent_index,
    Array<uint8_t> bitmap,
    size_t *bitmap_index,
    size_t *bitmap_sub_bit_index
) {
    if(parent_table[parent_index].present) {
        if(*current_table == nullptr || parent_index != *current_parent_index) {
            if(*current_table != nullptr) {
                unmap_memory(*current_table, sizeof(PageTableEntry[page_table_length]));

                *current_table = nullptr;
            }

            size_t logical_page_index;
            if(!map_pages(
                parent_table[parent_index].page_address,
                1,
                bitmap,
                &logical_page_index
            )) {
                return false;
            }

            *current_table = (PageTableEntry*)(logical_page_index * page_size);
            *current_parent_index = parent_index;
        }
    } else {
        if(*current_table != nullptr) {
            unmap_memory(*current_table, sizeof(PageTableEntry[page_table_length]));

            *current_table = nullptr;
        }

        size_t physical_page_index;
        if(!allocate_next_physical_page(
            bitmap_index,
            bitmap_sub_bit_index,
            bitmap,
            &physical_page_index
        )) {
            return false;
        }

        size_t logical_page_index;
        if(!map_pages(
            physical_page_index,
            1,
            bitmap,
            &logical_page_index
        )) {
            return false;
        }

        *current_table = (PageTableEntry*)(logical_page_index * page_size);
        *current_parent_index = parent_index;

        memset(*current_table, 0, sizeof(PageTableEntry[page_table_length]));

        parent_table[parent_index].present = true;
        parent_table[parent_index].write_allowed = true;
        parent_table[parent_index].user_mode_allowed = true;
        parent_table[parent_index].page_address = physical_page_index;
    }

    return true;
}

bool map_pages(
    size_t physical_pages_start,
    size_t page_count,
    UserPermissions permissions,
    size_t pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *logical_pages_start
) {
    if(!find_free_logical_pages(page_count, pml4_table_physical_address, bitmap, logical_pages_start)) {
        return false;
    }

    auto pml4_table = (PageTableEntry*)map_memory(
        pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap
    );
    if(pml4_table == nullptr) {
        return false;
    }

    size_t current_pml4_index;
    PageTableEntry *pdp_table = nullptr;

    size_t current_pdp_index;
    PageTableEntry *pd_table = nullptr;

    size_t current_pd_index;
    PageTableEntry *page_table = nullptr;

    size_t bitmap_index = 0;
    size_t bitmap_sub_bit_index = 0;

    for(
        size_t relative_page_index = 0;
        relative_page_index < page_count;
        relative_page_index += 1
    ) {
        auto page_index = *logical_pages_start + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        if(
            !map_and_maybe_allocate_table(
                &current_pml4_index,
                &pdp_table,
                pml4_table,
                pml4_index,
                bitmap,
                &bitmap_index,
                &bitmap_sub_bit_index
            ) ||
            !map_and_maybe_allocate_table(
                &current_pdp_index,
                &pd_table,
                pdp_table,
                pdp_index,
                bitmap,
                &bitmap_index,
                &bitmap_sub_bit_index
            ) ||
            !map_and_maybe_allocate_table(
                &current_pd_index,
                &page_table,
                pd_table,
                pd_index,
                bitmap,
                &bitmap_index,
                &bitmap_sub_bit_index
            )
        ) {
            unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));

            if(pdp_table != nullptr) {
                unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(pd_table != nullptr) {
                unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(page_table != nullptr) {
                unmap_memory(page_table, sizeof(PageTableEntry[page_table_length]));
            }

            return false;
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = permissions & UserPermissions::Write;
        page_table[page_index].execute_disable = !(permissions & UserPermissions::Execute);
        page_table[page_index].user_mode_allowed = true;
        page_table[page_index].page_address = physical_pages_start + relative_page_index;
    }

    unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));

    if(pdp_table != nullptr) {
        unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(pd_table != nullptr) {
        unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(page_table != nullptr) {
        unmap_memory(page_table, sizeof(PageTableEntry[page_table_length]));
    }

    return true;
}

bool map_pages_from_kernel(
    size_t kernel_logical_pages_start,
    size_t page_count,
    UserPermissions permissions,
    size_t user_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *user_logical_pages_start
) {
    if(!find_free_logical_pages(page_count, user_pml4_table_physical_address, bitmap, user_logical_pages_start)) {
        return false;
    }

    auto user_pml4_table = (PageTableEntry*)map_memory(
        user_pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap
    );
    if(user_pml4_table == nullptr) {
        return false;
    }

    size_t current_user_pml4_index;
    PageTableEntry *user_pdp_table = nullptr;

    size_t current_user_pdp_index;
    PageTableEntry *user_pd_table = nullptr;

    size_t current_user_pd_index;
    PageTableEntry *user_page_table = nullptr;

    size_t bitmap_index = 0;
    size_t bitmap_sub_bit_index = 0;

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto kernel_page_index = kernel_logical_pages_start + relative_page_index;
        auto kernel_pd_index = kernel_page_index / page_table_length;
        auto kernel_pdp_index = kernel_pd_index / page_table_length;
        auto kernel_pml4_index = kernel_pdp_index / page_table_length;

        kernel_page_index %= page_table_length;
        kernel_pd_index %= page_table_length;
        kernel_pdp_index %= page_table_length;
        kernel_pml4_index %= page_table_length;

        auto user_page_index = *user_logical_pages_start + relative_page_index;
        auto user_pd_index = user_page_index / page_table_length;
        auto user_pdp_index = user_pd_index / page_table_length;
        auto user_pml4_index = user_pdp_index / page_table_length;

        user_page_index %= page_table_length;
        user_pd_index %= page_table_length;
        user_pdp_index %= page_table_length;
        user_pml4_index %= page_table_length;

        if(
            !map_and_maybe_allocate_table(
                &current_user_pml4_index,
                &user_pdp_table,
                user_pml4_table,
                user_pml4_index,
                bitmap,
                &bitmap_index,
                &bitmap_sub_bit_index
                
            ) ||
            !map_and_maybe_allocate_table(
                &current_user_pdp_index,
                &user_pd_table,
                user_pdp_table,
                user_pdp_index,
                bitmap,
                &bitmap_index,
                &bitmap_sub_bit_index
                
            ) ||
            !map_and_maybe_allocate_table(
                &current_user_pd_index,
                &user_page_table,
                user_pd_table,
                user_pd_index,
                bitmap,
                &bitmap_index,
                &bitmap_sub_bit_index
                
            )
        ) {
            unmap_memory(user_pml4_table, sizeof(PageTableEntry[page_table_length]));

            if(user_pdp_table != nullptr) {
                unmap_memory(user_pdp_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(user_pd_table != nullptr) {
                unmap_memory(user_pd_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(user_page_table != nullptr) {
                unmap_memory(user_page_table, sizeof(PageTableEntry[page_table_length]));
            }

            return false;
        }

        auto kernel_page_table = get_page_table_pointer(kernel_pml4_index, kernel_pdp_index, kernel_pd_index);

        user_page_table[user_page_index].present = true;
        user_page_table[user_page_index].write_allowed = permissions & UserPermissions::Write;
        user_page_table[user_page_index].execute_disable = !(permissions & UserPermissions::Execute);
        user_page_table[user_page_index].user_mode_allowed = true;
        user_page_table[user_page_index].page_address = kernel_page_table[kernel_page_index].page_address;
    }

    unmap_memory(user_pml4_table, sizeof(PageTableEntry[page_table_length]));

    if(user_pdp_table != nullptr) {
        unmap_memory(user_pdp_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(user_pd_table != nullptr) {
        unmap_memory(user_pd_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(user_page_table != nullptr) {
        unmap_memory(user_page_table, sizeof(PageTableEntry[page_table_length]));
    }

    return true;
}

static bool map_table(
    size_t *current_parent_index,
    PageTableEntry **current_table,
    PageTableEntry *parent_table,
    size_t parent_index,
    Array<uint8_t> bitmap
) {
    if(*current_table == nullptr || parent_index != *current_parent_index) {
        if(*current_table != nullptr) {
            unmap_memory(*current_table, sizeof(PageTableEntry[page_table_length]));

            *current_table = nullptr;
        }

        size_t logical_page_index;
        if(!map_pages(
            parent_table[parent_index].page_address,
            1,
            bitmap,
            &logical_page_index
        )) {
            return false;
        }

        *current_table = (PageTableEntry*)(logical_page_index * page_size);
        *current_parent_index = parent_index;
    }

    return true;
}

bool map_pages_from_user(
    size_t user_logical_pages_start,
    size_t page_count,
    size_t user_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *kernel_logical_pages_start
) {
    if(!find_free_logical_pages(page_count, kernel_logical_pages_start)) {
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

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

        page_table[page_index].present = true;
    }

    auto user_pml4_table = (PageTableEntry*)map_memory(
        user_pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap
    );
    if(user_pml4_table == nullptr) {
        return false;
    }

    size_t current_user_pml4_index;
    PageTableEntry *user_pdp_table = nullptr;

    size_t current_user_pdp_index;
    PageTableEntry *user_pd_table = nullptr;

    size_t current_user_pd_index;
    PageTableEntry *user_page_table = nullptr;

    size_t bitmap_index = 0;
    size_t bitmap_sub_bit_index = 0;

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto kernel_page_index = *kernel_logical_pages_start + relative_page_index;
        auto kernel_pd_index = kernel_page_index / page_table_length;
        auto kernel_pdp_index = kernel_pd_index / page_table_length;
        auto kernel_pml4_index = kernel_pdp_index / page_table_length;

        kernel_page_index %= page_table_length;
        kernel_pd_index %= page_table_length;
        kernel_pdp_index %= page_table_length;
        kernel_pml4_index %= page_table_length;

        auto user_page_index = user_logical_pages_start + relative_page_index;
        auto user_pd_index = user_page_index / page_table_length;
        auto user_pdp_index = user_pd_index / page_table_length;
        auto user_pml4_index = user_pdp_index / page_table_length;

        user_page_index %= page_table_length;
        user_pd_index %= page_table_length;
        user_pdp_index %= page_table_length;
        user_pml4_index %= page_table_length;

        if(
            !map_table(
                &current_user_pml4_index,
                &user_pdp_table,
                user_pml4_table,
                user_pml4_index,
                bitmap
            ) ||
            !map_table(
                &current_user_pdp_index,
                &user_pd_table,
                user_pdp_table,
                user_pdp_index,
                bitmap
            ) ||
            !map_table(
                &current_user_pd_index,
                &user_page_table,
                user_pd_table,
                user_pd_index,
                bitmap
            )
        ) {
            unmap_memory(user_pml4_table, sizeof(PageTableEntry[page_table_length]));

            if(user_pdp_table != nullptr) {
                unmap_memory(user_pdp_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(user_pd_table != nullptr) {
                unmap_memory(user_pd_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(user_page_table != nullptr) {
                unmap_memory(user_page_table, sizeof(PageTableEntry[page_table_length]));
            }

            return false;
        }

        auto kernel_page_table = get_page_table_pointer(kernel_pml4_index, kernel_pdp_index, kernel_pd_index);

        kernel_page_table[kernel_page_index].write_allowed = true;
        kernel_page_table[kernel_page_index].user_mode_allowed = false;
        kernel_page_table[kernel_page_index].page_address = user_page_table[user_page_index].page_address;
    }

    unmap_memory(user_pml4_table, sizeof(PageTableEntry[page_table_length]));

    if(user_pdp_table != nullptr) {
        unmap_memory(user_pdp_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(user_pd_table != nullptr) {
        unmap_memory(user_pd_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(user_page_table != nullptr) {
        unmap_memory(user_page_table, sizeof(PageTableEntry[page_table_length]));
    }

    return true;
}

bool map_pages_between_user(
    size_t from_logical_pages_start,
    size_t page_count,
    UserPermissions permissions,
    size_t from_pml4_table_physical_address,
    size_t to_pml4_table_physical_address,
    Array<uint8_t> bitmap,
    size_t *to_logical_pages_start
) {
    if(!find_free_logical_pages(page_count, to_pml4_table_physical_address, bitmap, to_logical_pages_start)) {
        return false;
    }

    auto from_pml4_table = (PageTableEntry*)map_memory(
        from_pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap
    );
    if(from_pml4_table == nullptr) {
        return false;
    }

    size_t current_from_pml4_index;
    PageTableEntry *from_pdp_table = nullptr;

    size_t current_from_pdp_index;
    PageTableEntry *from_pd_table = nullptr;

    size_t current_from_pd_index;
    PageTableEntry *from_page_table = nullptr;

    auto to_pml4_table = (PageTableEntry*)map_memory(
        to_pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap
    );
    if(to_pml4_table == nullptr) {
        return false;
    }

    size_t current_to_pml4_index;
    PageTableEntry *to_pdp_table = nullptr;

    size_t current_to_pdp_index;
    PageTableEntry *to_pd_table = nullptr;

    size_t current_to_pd_index;
    PageTableEntry *to_page_table = nullptr;

    size_t bitmap_index = 0;
    size_t bitmap_sub_bit_index = 0;

    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
        auto from_page_index = from_logical_pages_start + relative_page_index;
        auto from_pd_index = from_page_index / page_table_length;
        auto from_pdp_index = from_pd_index / page_table_length;
        auto from_pml4_index = from_pdp_index / page_table_length;

        from_page_index %= page_table_length;
        from_pd_index %= page_table_length;
        from_pdp_index %= page_table_length;
        from_pml4_index %= page_table_length;

        auto to_page_index = *to_logical_pages_start + relative_page_index;
        auto to_pd_index = to_page_index / page_table_length;
        auto to_pdp_index = to_pd_index / page_table_length;
        auto to_pml4_index = to_pdp_index / page_table_length;

        to_page_index %= page_table_length;
        to_pd_index %= page_table_length;
        to_pdp_index %= page_table_length;
        to_pml4_index %= page_table_length;

        if(
            !map_table(
                &current_from_pml4_index,
                &from_pdp_table,
                from_pml4_table,
                from_pml4_index,
                bitmap
            ) ||
            !map_table(
                &current_from_pdp_index,
                &from_pd_table,
                from_pdp_table,
                from_pdp_index,
                bitmap
            ) ||
            !map_table(
                &current_from_pd_index,
                &from_page_table,
                from_pd_table,
                from_pd_index,
                bitmap
            ) ||
            !map_table(
                &current_to_pml4_index,
                &to_pdp_table,
                to_pml4_table,
                to_pml4_index,
                bitmap
            ) ||
            !map_table(
                &current_to_pdp_index,
                &to_pd_table,
                to_pdp_table,
                to_pdp_index,
                bitmap
            ) ||
            !map_table(
                &current_to_pd_index,
                &to_page_table,
                to_pd_table,
                to_pd_index,
                bitmap
            )
        ) {
            unmap_memory(from_pml4_table, sizeof(PageTableEntry[page_table_length]));

            if(from_pdp_table != nullptr) {
                unmap_memory(from_pdp_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(from_pd_table != nullptr) {
                unmap_memory(from_pd_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(from_page_table != nullptr) {
                unmap_memory(from_page_table, sizeof(PageTableEntry[page_table_length]));
            }

            unmap_memory(to_pml4_table, sizeof(PageTableEntry[page_table_length]));

            if(to_pdp_table != nullptr) {
                unmap_memory(to_pdp_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(to_pd_table != nullptr) {
                unmap_memory(to_pd_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(to_page_table != nullptr) {
                unmap_memory(to_page_table, sizeof(PageTableEntry[page_table_length]));
            }

            return false;
        }

        to_page_table[to_page_index].write_allowed = true;
        to_page_table[to_page_index].write_allowed = permissions & UserPermissions::Write;
        to_page_table[to_page_index].execute_disable = !(permissions & UserPermissions::Execute);
        to_page_table[to_page_index].user_mode_allowed = true;
        to_page_table[to_page_index].page_address = from_page_table[from_page_index].page_address;
    }

    unmap_memory(from_pml4_table, sizeof(PageTableEntry[page_table_length]));

    if(from_pdp_table != nullptr) {
        unmap_memory(from_pdp_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(from_pd_table != nullptr) {
        unmap_memory(from_pd_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(from_page_table != nullptr) {
        unmap_memory(from_page_table, sizeof(PageTableEntry[page_table_length]));
    }

    unmap_memory(to_pml4_table, sizeof(PageTableEntry[page_table_length]));

    if(to_pdp_table != nullptr) {
        unmap_memory(to_pdp_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(to_pd_table != nullptr) {
        unmap_memory(to_pd_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(to_page_table != nullptr) {
        unmap_memory(to_page_table, sizeof(PageTableEntry[page_table_length]));
    }

    return true;
}

bool unmap_pages(
    size_t logical_pages_start,
    size_t page_count,
    size_t pml4_table_physical_address,
    bool deallocate,
    Array<uint8_t> bitmap
) {
    auto pml4_table = (PageTableEntry*)map_memory(
        pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap
    );
    if(pml4_table == nullptr) {
        return false;
    }

    size_t current_pml4_index;
    PageTableEntry *pdp_table = nullptr;

    size_t current_pdp_index;
    PageTableEntry *pd_table = nullptr;

    size_t current_pd_index;
    PageTableEntry *page_table = nullptr;

    for(
        size_t absolute_page_index = logical_pages_start;
        absolute_page_index < logical_pages_start + page_count;
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

        if(
            !map_table(
                &current_pml4_index,
                &pdp_table,
                pml4_table,
                pml4_index,
                bitmap
            ) ||
            !map_table(
                &current_pdp_index,
                &pd_table,
                pdp_table,
                pdp_index,
                bitmap
            ) ||
            !map_table(
                &current_pd_index,
                &page_table,
                pd_table,
                pd_index,
                bitmap
            )
        ) {
            unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));

            if(pdp_table != nullptr) {
                unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(pd_table != nullptr) {
                unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));
            }

            if(page_table != nullptr) {
                unmap_memory(page_table, sizeof(PageTableEntry[page_table_length]));
            }

            return false;
        }

        page_table[page_index].present = false;

        if(deallocate) {
            auto physical_page_index = page_table[page_index].page_address;

            auto byte_index = physical_page_index / 8;
            auto sub_byte_index = physical_page_index % 8;

            bitmap[byte_index] &= ~(1 << sub_byte_index);
        }
    }

    unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));

    if(pdp_table != nullptr) {
        unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(pd_table != nullptr) {
        unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));
    }

    if(page_table != nullptr) {
        unmap_memory(page_table, sizeof(PageTableEntry[page_table_length]));
    }

    return true;
}