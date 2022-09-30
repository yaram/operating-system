#include "process.h"
#include "paging.h"
#include "memory.h"
#include "bucket_array_kernel.h"
#include "threading_kernel.h"
#include "multiprocessing.h"

#define bits_to_mask(bits) ((1 << (bits)) - 1)

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
    PagePermissions permissions,
    Process *process,
    Array<uint8_t> bitmap,
    size_t *user_pages_start,
    size_t *kernel_pages_start
) {
    if(!map_and_allocate_pages(page_count, bitmap, kernel_pages_start)) {
        return false;
    }

    if(!map_pages_from_kernel(
        *kernel_pages_start,
        page_count,
        permissions,
        process->pml4_table_physical_address,
        bitmap,
        user_pages_start
    )) {
        unmap_and_deallocate_pages(*kernel_pages_start, page_count, bitmap);

        return false;
    }

    register_process_mapping(process, *user_pages_start, page_count, false, true, bitmap);

    fill_memory((void*)(*kernel_pages_start * page_size), page_count * page_size, 0);

    return true;
}

struct SectionAllocation {
    size_t user_pages_start;
    size_t kernel_pages_start;

    size_t page_count;
};

using SectionAllocations = BucketArray<SectionAllocation, 16>;

static const SectionAllocation *get_section_allocation(
    size_t section_index,
    ConstArray<ELFSectionHeader> section_headers,
    const SectionAllocations *allocations
) {
    auto allocation_iterator = begin(*allocations);
    for(size_t i = 0; i < section_index; i += 1) {
        auto section_header = &section_headers[i];

        if((section_header->flags & 0b10) != 0) { // SHF_ALLOC
            ++allocation_iterator;
        }
    }

    return *allocation_iterator;
}

CreateProcessFromELFResult create_process_from_elf(
    uint8_t *elf_binary,
    size_t elf_binary_size,
    void *data,
    size_t data_size,
    Array<uint8_t> bitmap,
    size_t processor_area_count,
    size_t processor_areas_physical_memory_start,
    Processes *processes,
    Process **result_processs,
    Processes::Iterator *result_process_iterator
) {
    Processes::Iterator process_iterator;
    auto process = allocate_from_bucket_array(processes, bitmap, true, &process_iterator);
    if(process == nullptr) {
        return CreateProcessFromELFResult::OutOfMemory;
    }

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
        remove_item_from_bucket_array(process_iterator);

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

    { // Initalize process page tables with kernel pages
        auto pml4_table = (PageTableEntry*)map_memory(
            process->pml4_table_physical_address,
            sizeof(PageTableEntry[page_table_length]),
            bitmap
        );
        if(pml4_table == nullptr) {
            remove_item_from_bucket_array(process_iterator);

            return CreateProcessFromELFResult::OutOfMemory;
        }

        fill_memory(pml4_table, sizeof(PageTableEntry[page_table_length]), 0);

        PageWalker walker {};
        walker.absolute_page_index = kernel_pages_start;
        walker.pml4_table = pml4_table;

        for(size_t absolute_page_index = kernel_pages_start; absolute_page_index < kernel_pages_end; absolute_page_index += 1) {
            if(!increment_page_walker(&walker, bitmap)) {
                unmap_page_walker(&walker);

                remove_item_from_bucket_array(process_iterator);

                return CreateProcessFromELFResult::OutOfMemory;
            }

            auto page = &walker.page_table[walker.page_index];

            page->present = true;
            page->write_allowed = true;
            page->user_mode_allowed = false;
            page->page_address = absolute_page_index;
        }

        unmap_page_walker(&walker);
    }

    auto processor_areas_page_count = divide_round_up(processor_area_count * sizeof(ProcessorArea), page_size);
    auto processor_areas_physical_pages_start = processor_areas_physical_memory_start / page_size;

    { // Map pages for processor area
        PageWalker walker;
        if(!create_page_walker(process->pml4_table_physical_address, user_processor_areas_pages_start, bitmap, &walker)) {
            printf("Error: Out of memory\n");

            halt();
        }

        for(size_t relative_page_index = 0; relative_page_index < processor_areas_page_count; relative_page_index += 1) {
            if(!increment_page_walker(&walker, bitmap)) {
                printf("Error: Out of memory\n");

                halt();
            }

            auto page = &walker.page_table[walker.page_index];

            page->present = true;
            page->write_allowed = true;
            page->user_mode_allowed = false;
            page->page_address = processor_areas_physical_pages_start + relative_page_index;
        }

        unmap_page_walker(&walker);
    }

    SectionAllocations section_allocations {};

    {
        for(size_t i = 0; i < section_headers.length; i += 1) {
            auto section_header = &section_headers[i];

            if((section_header->flags & 0b10) != 0) { // SHF_ALLOC
                auto page_count = divide_round_up(section_header->size, page_size);

                PagePermissions permissions {};

                if((section_header->flags & 0b1) != 0) { // SHF_WRITE
                    permissions = (PagePermissions)(permissions | PagePermissions::Write);
                }

                if((section_header->flags & 0b100) != 0) { // SHF_EXECINSTR
                    permissions = (PagePermissions)(permissions | PagePermissions::Execute);
                }

                size_t user_pages_start;
                size_t kernel_pages_start;
                if(!map_and_allocate_pages_in_process_and_kernel(
                    page_count,
                    permissions,
                    process,
                    bitmap,
                    &user_pages_start,
                    &kernel_pages_start
                )) {
                    destroy_process(process_iterator, bitmap);
                    unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

                    return CreateProcessFromELFResult::OutOfMemory;
                }

                auto section_allocation = allocate_from_bucket_array(&section_allocations, bitmap, false);
                if(section_allocation == nullptr) {
                    destroy_process(process_iterator, bitmap);
                    unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

                    return CreateProcessFromELFResult::OutOfMemory;
                }

                *section_allocation = {
                    user_pages_start,
                    kernel_pages_start,
                    page_count
                };

                if((section_header->flags & 0b100) != 0) { // SHF_EXECINSTR
                    auto debug_code_section = allocate_from_bucket_array(&process->debug_code_sections, bitmap, true);
                    if(section_allocation == nullptr) {
                        destroy_process(process_iterator, bitmap);
                        unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

                        return CreateProcessFromELFResult::OutOfMemory;
                    }

                    debug_code_section->memory_start = user_pages_start * page_size;
                    debug_code_section->size = page_count * page_size;

                    for(size_t i = 0; i < DebugCodeSection::name_buffer_length; i += 1) {
                        auto character = section_names[section_header->name_offset + i];

                        if(character == 0) {
                            break;
                        }

                        debug_code_section->name_buffer[i] = character;

                        debug_code_section->name_length += 1;
                    }
                }
            }
        }
    }

    {
        auto section_allocation_iterator = begin(section_allocations);

        for(size_t i = 0; i < section_headers.length; i += 1) {
            auto section_header = &section_headers[i];

            if((section_header->flags & 0b10) != 0) { // SHF_ALLOC
                if(section_header->type != 8) { // SHT_NOBITS
                    auto allocation = *section_allocation_iterator;

                    copy_memory(
                        (void*)((size_t)elf_binary + section_header->in_file_offset),
                        (void*)(allocation->kernel_pages_start * page_size),
                        section_header->size
                    );
                }

                ++section_allocation_iterator;
            }
        }
    }

    // Global Offset Table is fixed-size for now, should expand dynamically at some point.

    const size_t global_offset_table_size = 4096;
    const size_t global_offset_table_page_count = divide_round_up(global_offset_table_size, page_size);

    size_t global_offset_table_user_pages_start;
    size_t global_offset_table_kernel_pages_start;
    if(!map_and_allocate_pages_in_process_and_kernel(
        global_offset_table_page_count,
        {},
        process,
        bitmap,
        &global_offset_table_user_pages_start,
        &global_offset_table_kernel_pages_start
    )) {
        destroy_process(process_iterator, bitmap);
        unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

        return CreateProcessFromELFResult::OutOfMemory;
    }

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

            auto slot_section_allocation = get_section_allocation(slot_section_index, section_headers, &section_allocations);

            ConstArray<ELFRelocationAddend> relocations {
                (const ELFRelocationAddend*)((size_t)elf_binary + section_header->in_file_offset),
                section_header->size / sizeof(ELFRelocationAddend),
            };

            auto slot_section_kernel_address = slot_section_allocation->kernel_pages_start * page_size;
            auto slot_section_user_address = slot_section_allocation->user_pages_start * page_size;

            for(size_t j = 0; j < relocations.length; j += 1) {
                auto relocation = &relocations[j];

                auto symbol = &symbols[relocation->symbol];

                auto symbol_user_address = symbol->value;
                if(symbol->section_index != 0) {
                    auto symbol_section_allocation = get_section_allocation(symbol->section_index, section_headers, &section_allocations);

                    symbol_user_address += symbol_section_allocation->user_pages_start * page_size;
                }

                auto slot_relative_address = relocation->offset;

                auto slot_kernel_address = slot_section_kernel_address + slot_relative_address;
                auto slot_user_address = slot_section_user_address + slot_relative_address;

                switch(relocation->type) {
                    case 0: break; // R_X86_64_NONE

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
                            unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

                            return CreateProcessFromELFResult::OutOfMemory;
                        }

                        global_offset_table[index] = symbol_user_address;

                        auto offset = index * sizeof(size_t);

                        *(uint32_t*)slot_kernel_address = (uint32_t)(offset + relocation->addend);
                    } break;

                    case 4: { // R_X86_64_PLT32
                        *(uint32_t*)slot_kernel_address = (uint32_t)(symbol_user_address + relocation->addend - slot_user_address);
                    } break;

                    case 9: // R_X86_64_GOTPCREL
                    case 41: // R_X86_64_GOTPCRELX
                    case 42: { // R_X86_64_REX_GOTPCRELX
                        auto index = next_global_offset_table_index;
                        next_global_offset_table_index += 1;

                        if(index == global_offset_table.length) {
                            destroy_process(process_iterator, bitmap);
                            unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

                            return CreateProcessFromELFResult::OutOfMemory;
                        }

                        global_offset_table[index] = symbol_user_address;

                        auto offset = index * sizeof(size_t);

                        *(uint32_t*)slot_kernel_address = (uint32_t)(
                            offset +
                            global_offset_table_address +
                            relocation->addend -
                            slot_user_address
                        );
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
                            unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

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
                        unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

                        return CreateProcessFromELFResult::InvalidELF;
                    } break;
                }
            }
        }
    }

    unmap_pages(global_offset_table_kernel_pages_start, global_offset_table_page_count);

    for(auto allocation : section_allocations) {
        unmap_pages(allocation->kernel_pages_start, allocation->page_count);
    }

    const size_t stack_size = 1024 * 16;
    const size_t stack_page_count = divide_round_up(stack_size, page_size);

    size_t stack_user_pages_start;
    size_t stack_kernel_pages_start;
    if(!map_and_allocate_pages_in_process_and_kernel(
        stack_page_count,
        PagePermissions::Write,
        process,
        bitmap,
        &stack_user_pages_start,
        &stack_kernel_pages_start
    )) {
        destroy_process(process_iterator, bitmap);
        unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

        return CreateProcessFromELFResult::OutOfMemory;
    }

    unmap_pages(stack_kernel_pages_start, stack_page_count);

    auto stack_top = (void*)(stack_user_pages_start * page_size + stack_size);

    auto data_page_count = divide_round_up(data_size, page_size);

    size_t data_user_pages_start;
    size_t data_kernel_pages_start;
    if(data_size == 0 || data == nullptr) {
        data_user_pages_start = 0;
    } else {
        if(!map_and_allocate_pages_in_process_and_kernel(
            data_page_count,
            PagePermissions::Write,
            process,
            bitmap,
            &data_user_pages_start,
            &data_kernel_pages_start
        )) {
            destroy_process(process_iterator, bitmap);
            unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

            return CreateProcessFromELFResult::OutOfMemory;
        }

        copy_memory(data, (void*)(data_kernel_pages_start * page_size), data_size);

        unmap_pages(data_kernel_pages_start, data_page_count);
    }

    auto entry_section_allocation = get_section_allocation(entry_symbol->section_index, section_headers, &section_allocations);

    auto entry_point = (void*)(entry_section_allocation->user_pages_start * page_size + entry_symbol->value);

    unmap_and_deallocate_bucket_array(&section_allocations, bitmap);

    auto thread = allocate_from_bucket_array(&process->threads, bitmap, true);

    // Set process entry conditions
    thread->frame.interrupt_frame.instruction_pointer = entry_point;
    thread->frame.interrupt_frame.code_segment = 0x23;
    thread->frame.interrupt_frame.cpu_flags = 1 << 9;
    thread->frame.interrupt_frame.stack_pointer = (void*)((size_t)stack_top - 8);
    thread->frame.interrupt_frame.stack_segment = 0x1B;

    // Set entry function parameters (process ID, data & data-size)
    thread->frame.rdi = process->id;
    thread->frame.rsi = data_user_pages_start * page_size;
    thread->frame.rdx = data_size;

    // Set ABI-specified intial register states

    thread->frame.mxcsr |= bits_to_mask(6) << 7;

    thread->is_ready = true;
    process->is_ready = true;

    *result_processs = process;
    *result_process_iterator = process_iterator;
    return CreateProcessFromELFResult::Success;
}

inline void deallocate_page(size_t page_index, Array<uint8_t> bitmap) {
    auto byte_index = page_index / 8;
    auto sub_byte_index = page_index % 8;

    bitmap[byte_index] &= ~(1 << sub_byte_index);
}

bool destroy_process(Processes::Iterator iterator, Array<uint8_t> bitmap) {
    remove_item_from_bucket_array(iterator);

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
    unmap_and_deallocate_bucket_array(&process->mappings, bitmap);

    // Deallocate the debug sections
    unmap_and_deallocate_bucket_array(&process->debug_code_sections, bitmap);

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
    bool is_shared,
    bool is_owned,
    Array<uint8_t> bitmap
) {
    auto page_mapping = allocate_from_bucket_array(&process->mappings, bitmap, true);
    if(page_mapping == nullptr) {
        return false;
    }

    *page_mapping = {
        logical_pages_start,
        page_count,
        is_shared,
        is_owned
    };

    return true;
}