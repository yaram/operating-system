#include "process.h"
#include "paging.h"
#include "memory.h"

struct ELFHeader {
    struct {
        uint8_t magic[4];
        uint8_t format;
        uint8_t data_type;
        uint8_t version;
        uint8_t abi;
        uint8_t abi_version;
        uint8_t unused[7];
    } identity;

    uint16_t type;
    uint16_t isa;
    uint8_t version;
    size_t entry_point;
    size_t program_header_offset;
    size_t section_header_offset;
    uint32_t flags;
    uint16_t header_size;
    uint16_t program_header_size;
    uint16_t program_header_count;
    uint16_t section_header_size;
    uint16_t section_header_count;
    uint16_t section_names_entry_index;
};

struct ELFProgramHeader {
    uint32_t type;
    uint32_t flags;
    size_t offset;
    size_t virtual_address;
    size_t physical_address;
    size_t in_file_size;
    size_t in_memory_size;
    size_t alignment;
};

struct ELFSectionHeader {
    uint32_t name_offset;
    uint32_t type;
    size_t flags;
    size_t address;
    size_t in_file_offset;
    size_t in_file_size;
    uint32_t link;
    uint32_t info;
    size_t alignment;
    size_t entry_size;
};

static bool map_and_maybe_allocate_table(
    size_t *current_parent_index,
    PageTableEntry **current_table,
    PageTableEntry *parent_table,
    size_t parent_index,
    uint8_t *bitmap_entries,
    size_t bitmap_size,
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
                bitmap_entries,
                bitmap_size,
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
            bitmap_entries,
            bitmap_size,
            &physical_page_index
        )) {
            return false;
        }

        size_t logical_page_index;
        if(!map_pages(
            physical_page_index,
            1,
            bitmap_entries,
            bitmap_size,
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

size_t next_process_id = 0;

bool create_process_from_elf(
    uint8_t *elf_binary,
    uint8_t *bitmap_entries,
    size_t bitmap_size,
    Processes *processes,
    Process **result_processs,
    Processes::Iterator *result_process_iterator
) {
    auto process_iterator = find_unoccupied_bucket_slot(processes);

    if(process_iterator.current_bucket == nullptr) {
        auto new_bucket = (Processes::Bucket*)map_and_allocate_memory(sizeof(Processes::Bucket), bitmap_entries, bitmap_size);
        if(new_bucket == nullptr) {
            return false;
        }

        memset((void*)new_bucket, 0, sizeof(Processes::Bucket));

        {
            auto current_bucket = &processes->first_bucket;
            while(current_bucket->next != nullptr) {
                current_bucket = current_bucket->next;
            }

            current_bucket->next = new_bucket;
        }

        process_iterator = {
            new_bucket,
            0
        };
    }

    process_iterator.current_bucket->occupied[process_iterator.current_sub_index] = true;

    auto process = *process_iterator;

    memset((void*)process, 0, sizeof(Process));

    process->id = next_process_id;
    next_process_id += 1;

    size_t bitmap_index = 0;
    size_t bitmap_sub_bit_index = 0;

    size_t pml4_physical_page_index;
    if(!allocate_next_physical_page(
        &bitmap_index,
        &bitmap_sub_bit_index,
        bitmap_entries,
        bitmap_size,
        &pml4_physical_page_index
    )) {
        process_iterator.current_bucket->occupied[process_iterator.current_sub_index] = false;

        return false;
    }

    process->pml4_table_physical_address = pml4_physical_page_index * page_size;

    // Currently assumes correct and specific elf header & content, no validation is done.

    auto elf_header = (ELFHeader*)elf_binary;

    auto program_headers = (ELFProgramHeader*)((size_t)elf_binary + elf_header->program_header_offset);

    size_t program_memory_size = 0;
    for(size_t i = 0; i < elf_header->program_header_count; i += 1) {
        auto end = program_headers[i].virtual_address + program_headers[i].in_memory_size;

        if(end > program_memory_size) {
            program_memory_size = end;
        }
    }

    { // Initalize process page tables with kernel pages
        auto pml4_table = (PageTableEntry*)map_memory(
            pml4_physical_page_index * page_size,
            sizeof(PageTableEntry[page_table_length]),
            bitmap_entries,
            bitmap_size
        );
        if(pml4_table == nullptr) {
            process_iterator.current_bucket->occupied[process_iterator.current_sub_index] = false;

            return false;
        }

        memset(pml4_table, 0, sizeof(PageTableEntry[page_table_length]));

        size_t current_pml4_index;
        PageTableEntry *pdp_table = nullptr;

        size_t current_pdp_index;
        PageTableEntry *pd_table = nullptr;

        size_t current_pd_index;
        PageTableEntry *page_table = nullptr;

        for(size_t absolute_page_index = kernel_pages_start; absolute_page_index < kernel_pages_end; absolute_page_index += 1) {
            auto page_index = absolute_page_index;
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
                    bitmap_entries,
                    bitmap_size,
                    &bitmap_index,
                    &bitmap_sub_bit_index
                ) ||
                !map_and_maybe_allocate_table(
                    &current_pdp_index,
                    &pd_table,
                    pdp_table,
                    pdp_index,
                    bitmap_entries,
                    bitmap_size,
                    &bitmap_index,
                    &bitmap_sub_bit_index
                ) ||
                !map_and_maybe_allocate_table(
                    &current_pd_index,
                    &page_table,
                    pd_table,
                    pd_index,
                    bitmap_entries,
                    bitmap_size,
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

                destroy_process(process_iterator, bitmap_entries, bitmap_size);

                return false;
            }

            page_table[page_index].present = true;
            page_table[page_index].write_allowed = true;
            page_table[page_index].user_mode_allowed = true;
            page_table[page_index].page_address = absolute_page_index;
        }

        unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));
    }

    auto process_page_count = divide_round_up(program_memory_size, page_size);

    size_t process_user_pages_start;
    if(!find_free_logical_pages(
        process_page_count,
        process->pml4_table_physical_address,
        bitmap_entries,
        bitmap_index,
        &process_user_pages_start
    )) {
        destroy_process(process_iterator, bitmap_entries, bitmap_size);

        return false;
    }

    if(!register_process_allocation(process, process_user_pages_start, process_page_count, bitmap_entries, bitmap_size)) {
        destroy_process(process_iterator, bitmap_entries, bitmap_size);

        return false;
    }

    process->logical_pages_start = process_user_pages_start;
    process->page_count = process_page_count;

    size_t process_kernel_pages_start;
    if(!find_free_logical_pages(process_page_count, &process_kernel_pages_start)) {
        destroy_process(process_iterator, bitmap_entries, bitmap_size);

        return false;
    }

    for(size_t j = 0; j < process_page_count; j += 1) {
        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap_entries,
            bitmap_size,
            &physical_page_index
        )){
            destroy_process(process_iterator, bitmap_entries, bitmap_size);

            return false;
        }

        // The order of these is IMPORTANT, otherwise the kernel pages will get overwritten!!!

        if(!set_page(process_user_pages_start + j, physical_page_index, process->pml4_table_physical_address, bitmap_entries, bitmap_size)) {
            destroy_process(process_iterator, bitmap_entries, bitmap_size);

            return false;
        }

        if(!set_page(process_kernel_pages_start + j, physical_page_index, bitmap_entries, bitmap_size)) {
            destroy_process(process_iterator, bitmap_entries, bitmap_size);

            return false;
        }
    }

    auto process_kernel_memory_start = process_kernel_pages_start * page_size;

    memset((void*)process_kernel_memory_start, 0, process_page_count * page_size);

    for(size_t i = 0; i < elf_header->program_header_count; i += 1) {
        if(program_headers[i].type == 1) { // Loadable segment
            memcpy(
                (void*)(process_kernel_memory_start + program_headers[i].virtual_address),
                (void*)((size_t)elf_binary + program_headers[i].offset),
                program_headers[i].in_file_size
            );
        }
    }

    unmap_pages(process_kernel_pages_start, process_page_count);

    const size_t process_stack_size = 4096;
    const size_t process_stack_page_count = divide_round_up(process_stack_size, page_size);

    size_t process_stack_pages_start;
    if(!find_free_logical_pages(
        process_stack_page_count,
        process->pml4_table_physical_address,
        bitmap_entries,
        bitmap_index,
        &process_stack_pages_start
    )) {
        destroy_process(process_iterator, bitmap_entries, bitmap_size);

        return false;
    }

    if(!register_process_allocation(process, process_stack_pages_start, process_page_count, bitmap_entries, bitmap_size)) {
        destroy_process(process_iterator, bitmap_entries, bitmap_size);

        return false;
    }

    for(size_t j = 0; j < process_stack_page_count; j += 1) {
        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap_entries,
            bitmap_size,
            &physical_page_index
        )){
            destroy_process(process_iterator, bitmap_entries, bitmap_size);

            return false;
        }

        if(!set_page(process_stack_pages_start + j, physical_page_index, process->pml4_table_physical_address, bitmap_entries, bitmap_size)) {
            destroy_process(process_iterator, bitmap_entries, bitmap_size);

            return false;
        }
    }

    memset((void*)(process_stack_pages_start * page_size), 0, process_stack_page_count * page_size);

    auto stack_top = (void*)(process_stack_pages_start * page_size + process_stack_size);

    auto entry_point = (void*)(process_user_pages_start * page_size + elf_header->entry_point);

    process->frame.interrupt_frame.instruction_pointer = entry_point;
    process->frame.interrupt_frame.code_segment = 0x23;
    process->frame.interrupt_frame.cpu_flags = 1 << 9;
    process->frame.interrupt_frame.stack_pointer = stack_top;
    process->frame.interrupt_frame.stack_segment = 0x1B;

    *result_processs = process;
    *result_process_iterator = process_iterator;
    return true;
}

inline void deallocate_page(size_t page_index, uint8_t *bitmap_entries) {
    auto byte_index = page_index / 8;
    auto sub_byte_index = page_index % 8;

    bitmap_entries[byte_index] &= ~(1 << sub_byte_index);
}

static bool map_table(
    size_t *current_parent_index,
    PageTableEntry **current_table,
    PageTableEntry *parent_table,
    size_t parent_index,
    uint8_t *bitmap_entries,
    size_t bitmap_size
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
            bitmap_entries,
            bitmap_size,
            &logical_page_index
        )) {
            return false;
        }

        *current_table = (PageTableEntry*)(logical_page_index * page_size);
        *current_parent_index = parent_index;
    }

    return true;
}

bool destroy_process(Processes::Iterator iterator, uint8_t *bitmap_entries, size_t bitmap_size) {
    iterator.current_bucket->occupied[iterator.current_sub_index] = false;

    auto process = *iterator;

    // Deallocate free memory allocations for process

    auto pml4_table = (PageTableEntry*)map_memory(
        process->pml4_table_physical_address,
        sizeof(PageTableEntry[page_table_length]),
        bitmap_entries,
        bitmap_size
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

    for(auto allocation : process->allocations) {
        for(
            size_t absolute_page_index = allocation->logical_pages_start;
            absolute_page_index < allocation->logical_pages_start + allocation->page_count;
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
                    bitmap_entries,
                    bitmap_size
                ) ||
                !map_table(
                    &current_pdp_index,
                    &pd_table,
                    pdp_table,
                    pdp_index,
                    bitmap_entries,
                    bitmap_size
                ) ||
                !map_table(
                    &current_pd_index,
                    &page_table,
                    pd_table,
                    pd_index,
                    bitmap_entries,
                    bitmap_size
                )
            ) {
                return false;
            }

            deallocate_page(page_table[page_index].page_address, bitmap_entries);
        }
    }

    // Deallocate the page tables themselves

    for(size_t pml4_index = 0; pml4_index < page_table_length; pml4_index += 1) {
        if(pml4_table[pml4_index].present) {
            auto pdp_table = (PageTableEntry*)map_memory(
                pml4_table[pml4_index].page_address * page_size,
                sizeof(PageTableEntry[page_table_length]),
                bitmap_entries,
                bitmap_size
            );
            if(pdp_table == nullptr) {
                return false;
            }

            for(size_t pdp_index = 0; pdp_index < page_table_length; pdp_index += 1) {
                if(pdp_table[pdp_index].present) {
                    auto pd_table = (PageTableEntry*)map_memory(
                        pdp_table[pdp_index].page_address * page_size,
                        sizeof(PageTableEntry[page_table_length]),
                        bitmap_entries,
                        bitmap_size
                    );
                    if(pd_table == nullptr) {
                        unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));

                        return false;
                    }

                    for(size_t pd_index = 0; pd_index < page_table_length; pd_index += 1) {
                        if(pd_table[pd_index].present) {
                            auto page_table = (PageTableEntry*)map_memory(
                                pd_table[pd_index].page_address * page_size,
                                sizeof(PageTableEntry[page_table_length]),
                                bitmap_entries,
                                bitmap_size
                            );
                            if(page_table == nullptr) {
                                unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
                                unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));

                                return false;
                            }

                            deallocate_page(pd_table[pd_index].page_address, bitmap_entries);

                            unmap_memory(page_table, sizeof(PageTableEntry[page_table_length]));
                        }
                    }

                    deallocate_page(pdp_table[pdp_index].page_address, bitmap_entries);

                    unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));
                }
            }

            deallocate_page(pml4_table[pml4_index].page_address, bitmap_entries);

            unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
        }
    }

    deallocate_page(process->pml4_table_physical_address / page_size, bitmap_entries);

    unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));

    return true;
}

bool register_process_allocation(Process *process, size_t logical_pages_start, size_t page_count, uint8_t *bitmap_entries, size_t bitmap_size) {
    auto allocation_iterator = find_unoccupied_bucket_slot(&process->allocations);

    if(allocation_iterator.current_bucket == nullptr) {
        auto new_bucket = (ProcessAllocations::Bucket*)map_and_allocate_memory(sizeof(ProcessAllocations::Bucket), bitmap_entries, bitmap_size);
        if(new_bucket == nullptr) {
            return false;
        }

        memset((void*)new_bucket, 0, sizeof(ProcessAllocations::Bucket));

        {
            auto current_bucket = &process->allocations.first_bucket;
            while(current_bucket->next != nullptr) {
                current_bucket = current_bucket->next;
            }

            current_bucket->next = new_bucket;
        }

        allocation_iterator = {
            new_bucket,
            0
        };
    }

    allocation_iterator.current_bucket->occupied[allocation_iterator.current_sub_index] = true;

    auto allocation = *allocation_iterator;

    *allocation = {
        logical_pages_start,
        page_count
    };

    return true;
}