#include "process.h"
#include "paging.h"
#include "memory.h"

#define align_round_up(value, alignment) divide_round_up(value, alignment) * alignment

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
    size_t size;
    uint32_t link;
    uint32_t info;
    size_t alignment;
    size_t entry_size;
};

struct ELFSymbol {
    uint32_t name_index;
    uint8_t type: 4;
    uint8_t bind: 4;
    uint8_t other;
    uint16_t section_index;
    size_t value;
    size_t size;
};

struct ELFRelocation {
    size_t offset;
    uint32_t type;
    uint32_t symbol;
};

struct ELFRelocationAddend {
    size_t offset;
    uint32_t type;
    uint32_t symbol;
    intptr_t addend;
};

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

size_t next_process_id = 0;

static bool c_string_equal(const char *a, const char *b) {
    size_t index = 0;
    while(true) {
        if(a[index] != b[index]) {
            return false;
        }

        if(a[index] == '\0') {
            return true;
        }

        index += 1;
    }
}

static bool map_and_allocate_pages_in_process_and_kernel(
    size_t page_count,
    Process *process,
    Processes::Iterator process_iterator,
    Array<uint8_t> bitmap,
    size_t *user_pages_start,
    size_t *kernel_pages_start
) {
    if(!find_free_logical_pages(
        page_count,
        process->pml4_table_physical_address,
        bitmap,
        user_pages_start
    )) {
        return false;
    }

    if(!register_process_mapping(process, *user_pages_start, page_count, true, bitmap)) {
        return false;
    }

    if(!find_free_logical_pages(page_count, kernel_pages_start)) {
        return false;
    }

    size_t bitmap_index = 0;
    size_t bitmap_sub_bit_index = 0;
    for(size_t i = 0; i < page_count; i += 1) {
        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap,
            &physical_page_index
        )){
            return false;
        }

        // The order of these is IMPORTANT, otherwise the kernel pages will get overwritten!!!

        if(!set_page(*user_pages_start + i, physical_page_index, process->pml4_table_physical_address, bitmap)) {
            return false;
        }

        if(!set_page(*kernel_pages_start + i, physical_page_index, bitmap)) {
            return false;
        }
    }

    return true;
}

static size_t get_section_relative_address(size_t section_index, ConstArray<ELFSectionHeader> section_headers) {
    size_t relative_address = 0;
    for(size_t i = 0; i < section_index; i += 1) {
        auto section_header = &section_headers[i];

        if((section_header->flags & 0b10) != 0) { // SHF_ALLOC
            relative_address = align_round_up(relative_address, section_header->alignment);

            relative_address += section_header->size;
        }
    }

    auto last_section_header = &section_headers[section_index];

    relative_address = align_round_up(relative_address, last_section_header->alignment);

    return relative_address;
}

inline void clear_pages(size_t page_index, size_t page_count) {
    memset((void*)(page_index * page_size), 0, page_count * page_size);
}

CreateProcessFromELFResult create_process_from_elf(
    uint8_t *elf_binary,
    Array<uint8_t> bitmap,
    Processes *processes,
    Process **result_processs,
    Processes::Iterator *result_process_iterator
) {
    auto process_iterator = find_unoccupied_bucket_slot(processes);

    if(process_iterator.current_bucket == nullptr) {
        auto new_bucket = (Processes::Bucket*)map_and_allocate_memory(sizeof(Processes::Bucket), bitmap);
        if(new_bucket == nullptr) {
            return CreateProcessFromELFResult::OutOfMemory;
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
        bitmap,
        &pml4_physical_page_index
    )) {
        process_iterator.current_bucket->occupied[process_iterator.current_sub_index] = false;

        return CreateProcessFromELFResult::OutOfMemory;
    }

    process->pml4_table_physical_address = pml4_physical_page_index * page_size;

    // Currently assumes correct and specific elf header & content, full validation is not done.

    auto elf_header = (ELFHeader*)elf_binary;

    if(elf_header->type != 1) { // ET_REL
        return CreateProcessFromELFResult::InvalidELF;
    }

    ConstArray<ELFSectionHeader> section_headers {
        (ELFSectionHeader*)((size_t)elf_binary + elf_header->section_header_offset),
        elf_header->section_header_count
    };

    ConstArray<char> section_names {
        (const char*)((size_t)elf_binary + section_headers[elf_header->section_names_entry_index].in_file_offset),
        section_headers[elf_header->section_names_entry_index].size
    };

    ConstArray<ELFSymbol> symbols;
    auto symbols_found = false;
    for(size_t i = 0; i < section_headers.length; i += 1) {
        auto section_header = &section_headers[i];

        if(section_header->type == 2) { // SHT_SYMTAB
            symbols.data = (ELFSymbol*)((size_t)elf_binary + section_header->in_file_offset);
            symbols.length = section_header->size / sizeof(ELFSymbol);

            symbols_found = true;
            break;
        }
    }

    if(!symbols_found) {
        return CreateProcessFromELFResult::InvalidELF;
    }

    ConstArray<char> symbol_names;
    auto symbol_names_found = false;
    for(size_t i = 0; i < section_headers.length; i += 1) {
        auto section_header = &section_headers[i];

        auto name = &section_names[section_header->name_offset];

        if(c_string_equal(name, ".strtab")) {
            symbol_names.data = (const char*)((size_t)elf_binary + section_header->in_file_offset);
            symbol_names.length = section_header->size;

            symbol_names_found = true;
            break;
        }
    }

    if(!symbol_names_found) {
        return CreateProcessFromELFResult::InvalidELF;
    }

    const auto entry_symbol_name = "entry";

    const ELFSymbol *entry_symbol;
    auto entry_symbol_found = false;
    for(size_t i = 0; i < symbols.length; i += 1) {
        auto symbol = &symbols[i];

        auto symbol_name = &symbol_names[symbol->name_index];

        if(c_string_equal(symbol_name, entry_symbol_name)) {
            entry_symbol = symbol;

            entry_symbol_found = true;
            break;
        }
    }

    if(!entry_symbol_found) {
        return CreateProcessFromELFResult::InvalidELF;
    }

    size_t image_size = 0;
    for(size_t i = 0; i < section_headers.length; i += 1) {
        auto section_header = &section_headers[i];

        if((section_header->flags & 0b10) != 0) { // SHF_ALLOC
            image_size = align_round_up(image_size, section_header->alignment);

            image_size += section_header->size;
        }
    }

    if(image_size == 0) {
        return CreateProcessFromELFResult::InvalidELF;
    }

    { // Initalize process page tables with kernel pages
        auto pml4_table = (PageTableEntry*)map_memory(
            pml4_physical_page_index * page_size,
            sizeof(PageTableEntry[page_table_length]),
            bitmap
        );
        if(pml4_table == nullptr) {
            process_iterator.current_bucket->occupied[process_iterator.current_sub_index] = false;

            return CreateProcessFromELFResult::OutOfMemory;
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

                destroy_process(process_iterator, bitmap);

                return CreateProcessFromELFResult::OutOfMemory;
            }

            page_table[page_index].present = true;
            page_table[page_index].write_allowed = true;
            page_table[page_index].user_mode_allowed = true;
            page_table[page_index].page_address = absolute_page_index;
        }

        unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));
    }

    auto image_page_count = divide_round_up(image_size, page_size);

    size_t image_user_pages_start;
    size_t image_kernel_pages_start;
    if(!map_and_allocate_pages_in_process_and_kernel(
        image_page_count,
        process,
        process_iterator,
        bitmap,
        &image_user_pages_start,
        &image_kernel_pages_start
    )) {
        destroy_process(process_iterator, bitmap);

        return CreateProcessFromELFResult::OutOfMemory;
    }

    auto image_kernel_memory_start = image_kernel_pages_start * page_size;

    clear_pages(image_kernel_pages_start, image_page_count);

    {
        auto section_relative_address = 0;

        // Calculate section offset, should cache this in DynamicArray and deallocate at the end of the function...
        for(size_t i = 0; i < section_headers.length; i += 1) {
            auto section_header = &section_headers[i];

            if((section_header->flags & 0b10) != 0) { // SHF_ALLOC
                section_relative_address = align_round_up(section_relative_address, section_header->alignment);

                if(section_header->type != 8) { // SHT_NOBITS
                    memcpy(
                        (void*)(image_kernel_memory_start + section_relative_address),
                        (void*)((size_t)elf_binary + section_header->in_file_offset),
                        section_header->size
                    );
                }

                section_relative_address += section_header->size;
            }
        }
    }

    auto image_user_memory_start = image_user_pages_start * page_size;

    // Global Offset Table memory is also used for the Procedure Translation Table,
    // will need to be separate when page read/write/execute protection is added.
    // Global Offset Table is fixed-size for now, should expand dynamically at some point.

    const size_t global_offset_table_size = 4096;
    const size_t global_offset_table_page_count = divide_round_up(global_offset_table_size, page_size);

    size_t global_offset_table_user_pages_start;
    size_t global_offset_table_kernel_pages_start;
    if(!map_and_allocate_pages_in_process_and_kernel(
        global_offset_table_page_count,
        process,
        process_iterator,
        bitmap,
        &global_offset_table_user_pages_start,
        &global_offset_table_kernel_pages_start
    )) {
        return CreateProcessFromELFResult::OutOfMemory;
    }

    clear_pages(global_offset_table_kernel_pages_start, global_offset_table_page_count);

    auto global_offset_table_address = global_offset_table_user_pages_start * page_size;

    Array<size_t> global_offset_table {
        (size_t*)(global_offset_table_kernel_pages_start * page_size),
        global_offset_table_size / sizeof(size_t)
    };

    size_t next_global_offset_table_index = 0;
    for(size_t i = 0; i < section_headers.length; i += 1) {
        auto section_header = &section_headers[i];

        if(section_header->type == 4) { // SHT_RELA
            auto slot_section_index = section_header->info;

            auto slot_section_header = &section_headers[slot_section_index];

            if((slot_section_header->flags & 0b10) == 0) { // !SHF_ALLOC
                continue;
            }

            auto slot_section_relative_address = get_section_relative_address(slot_section_index, section_headers);

            auto slot_section_kernel_address = image_kernel_memory_start + slot_section_relative_address;
            auto slot_section_user_address = image_user_memory_start + slot_section_relative_address;

            ConstArray<ELFRelocationAddend> relocations {
                (const ELFRelocationAddend*)((size_t)elf_binary + section_header->in_file_offset),
                section_header->size / sizeof(ELFRelocationAddend),
            };

            for(size_t j = 0; j < relocations.length; j += 1) {
                auto relocation = &relocations[j];

                auto symbol = &symbols[relocation->symbol];

                auto symbol_user_address = symbol->value;
                if(symbol->section_index != 0) {
                    auto symbol_section_relative_address = get_section_relative_address(symbol->section_index, section_headers);

                    symbol_user_address += image_user_memory_start + symbol_section_relative_address;
                }

                auto slot_relative_address = relocation->offset;

                auto slot_kernel_address = slot_section_kernel_address + slot_relative_address;
                auto slot_user_address = slot_section_user_address + slot_relative_address;

                switch(relocation->type) {
                    case 1: { // R_X86_64_64
                        *(uint64_t*)slot_kernel_address = symbol_user_address + relocation->addend;
                    } break;

                    case 2: { // R_X86_64_PC32
                        *(uint32_t*)slot_kernel_address = (uint32_t)(symbol_user_address + relocation->addend - slot_user_address);
                    } break;

                    case 3: { // R_X86_64_GOT32
                        auto index = next_global_offset_table_index;
                        next_global_offset_table_index += 1;

                        if(index == global_offset_table.length) {
                            destroy_process(process_iterator, bitmap);

                            return CreateProcessFromELFResult::OutOfMemory;
                        }

                        global_offset_table[index] = symbol_user_address;

                        auto offset = index * sizeof(size_t);

                        *(uint32_t*)slot_kernel_address = (uint32_t)(offset + relocation->addend);
                    } break;

                    case 4: { // R_X86_64_PLT32
                        *(uint32_t*)slot_kernel_address = (uint32_t)(symbol_user_address + relocation->addend - slot_user_address);
                    } break;

                    case 10: { // R_X86_64_32
                        *(uint32_t*)slot_kernel_address = (uint32_t)(symbol_user_address + relocation->addend);
                    } break;

                    case 24: { // R_X86_64_PC64
                        *(uint64_t*)slot_kernel_address = symbol_user_address + relocation->addend - slot_user_address;
                    } break;

                    case 25: { // R_X86_64_GOTOFF64
                        *(uint64_t*)slot_kernel_address = symbol_user_address + relocation->addend - global_offset_table_address;
                    } break;

                    case 27: { // R_X86_64_GOT64
                        auto index = next_global_offset_table_index;
                        next_global_offset_table_index += 1;

                        if(index == global_offset_table.length) {
                            destroy_process(process_iterator, bitmap);

                            return CreateProcessFromELFResult::OutOfMemory;
                        }

                        global_offset_table[index] = symbol_user_address;

                        auto offset = index * sizeof(size_t);

                        *(uint64_t*)slot_kernel_address = offset + relocation->addend;
                    } break;

                    case 26: { // R_X86_64_GOTPC32
                        *(uint32_t*)slot_kernel_address = (uint32_t)(global_offset_table_address + relocation->addend - slot_user_address);
                    } break;

                    case 29: { // R_X86_64_GOTPC64
                        *(uint64_t*)slot_kernel_address = global_offset_table_address + relocation->addend - slot_user_address;
                    } break;

                    default: {
                        destroy_process(process_iterator, bitmap);

                        return CreateProcessFromELFResult::InvalidELF;
                    } break;
                }
            }
        }
    }

    unmap_pages(global_offset_table_kernel_pages_start, global_offset_table_page_count);
    unmap_pages(image_kernel_pages_start, image_page_count);

    const size_t stack_size = 4096;
    const size_t stack_page_count = divide_round_up(stack_size, page_size);

    size_t stack_user_pages_start;
    size_t stack_kernel_pages_start;
    if(!map_and_allocate_pages_in_process_and_kernel(
        stack_page_count,
        process,
        process_iterator,
        bitmap,
        &stack_user_pages_start,
        &stack_kernel_pages_start
    )) {
        return CreateProcessFromELFResult::OutOfMemory;
    }

    clear_pages(stack_kernel_pages_start, stack_page_count);

    unmap_pages(stack_kernel_pages_start, stack_page_count);

    auto stack_top = (void*)(stack_user_pages_start * page_size + stack_size);

    auto entry_section_relative_address = get_section_relative_address(entry_symbol->section_index, section_headers);

    auto entry_point = (void*)(image_user_memory_start + entry_section_relative_address + entry_symbol->value);

    process->frame.interrupt_frame.instruction_pointer = entry_point;
    process->frame.interrupt_frame.code_segment = 0x23;
    process->frame.interrupt_frame.cpu_flags = 1 << 9;
    process->frame.interrupt_frame.stack_pointer = stack_top;
    process->frame.interrupt_frame.stack_segment = 0x1B;

    *result_processs = process;
    *result_process_iterator = process_iterator;
    return CreateProcessFromELFResult::Success;
}

inline void deallocate_page(size_t page_index, Array<uint8_t> bitmap) {
    auto byte_index = page_index / 8;
    auto sub_byte_index = page_index % 8;

    bitmap[byte_index] &= ~(1 << sub_byte_index);
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

bool destroy_process(Processes::Iterator iterator, Array<uint8_t> bitmap) {
    iterator.current_bucket->occupied[iterator.current_sub_index] = false;

    auto process = *iterator;

    // Deallocate owned memory mappings for process

    for(auto mapping : process->mappings) {
        if(!unmap_pages(
            mapping->logical_pages_start,
            mapping->page_count,
            process->pml4_table_physical_address,
            false,
            bitmap
        )) {
            return false;
        }
    }

    // Deallocate the memory mappings bucket array

    if(process->mappings.first_bucket.next != nullptr) {
        auto current_bucket = process->mappings.first_bucket.next;

        while(true) {
            auto next_bucket = current_bucket->next;

            unmap_and_deallocate_memory(current_bucket, sizeof(ProcessPageMappings::Bucket), bitmap);

            if(next_bucket == nullptr) {
                break;
            } else {
                current_bucket = next_bucket;
            }
        }
    }

    // Deallocate the page tables themselves

    auto pml4_table = (PageTableEntry*)map_memory(
        process->pml4_table_physical_address * page_size,
        sizeof(PageTableEntry[page_table_length]),
        bitmap
    );

    for(size_t pml4_index = 0; pml4_index < page_table_length; pml4_index += 1) {
        if(pml4_table[pml4_index].present) {
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
                if(pdp_table[pdp_index].present) {
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
                        if(pd_table[pd_index].present) {
                            deallocate_page(pd_table[pd_index].page_address, bitmap);
                        }
                    }

                    deallocate_page(pdp_table[pdp_index].page_address, bitmap);

                    unmap_memory(pd_table, sizeof(PageTableEntry[page_table_length]));
                }
            }

            deallocate_page(pml4_table[pml4_index].page_address, bitmap);

            unmap_memory(pdp_table, sizeof(PageTableEntry[page_table_length]));
        }
    }

    deallocate_page(process->pml4_table_physical_address / page_size, bitmap);

    unmap_memory(pml4_table, sizeof(PageTableEntry[page_table_length]));

    return true;
}

bool register_process_mapping(
    Process *process,
    size_t logical_pages_start,
    size_t page_count,
    bool is_owned,
    Array<uint8_t> bitmap
) {
    auto mapping_iterator = find_unoccupied_bucket_slot(&process->mappings);

    if(mapping_iterator.current_bucket == nullptr) {
        auto new_bucket = (ProcessPageMappings::Bucket*)map_and_allocate_memory(sizeof(ProcessPageMappings::Bucket), bitmap);
        if(new_bucket == nullptr) {
            return false;
        }

        memset((void*)new_bucket, 0, sizeof(ProcessPageMappings::Bucket));

        {
            auto current_bucket = &process->mappings.first_bucket;
            while(current_bucket->next != nullptr) {
                current_bucket = current_bucket->next;
            }

            current_bucket->next = new_bucket;
        }

        mapping_iterator = {
            new_bucket,
            0
        };
    }

    mapping_iterator.current_bucket->occupied[mapping_iterator.current_sub_index] = true;

    auto mapping = *mapping_iterator;

    *mapping = {
        logical_pages_start,
        page_count,
        is_owned
    };

    return true;
}