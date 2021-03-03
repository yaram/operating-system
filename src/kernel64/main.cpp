#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "acpi.h"
}
#include "console.h"
#include "heap.h"
#include "paging.h"
#include "process.h"
#include "memory.h"
#include "bucket_array.h"
#include "syscalls.h"
#include "pcie.h"

#define divide_round_up(dividend, divisor) (((dividend) + (divisor) - 1) / (divisor))

#define do_ranges_intersect(a_start, a_end, b_start, b_end) (!((a_end) <= (b_start) || (b_end) <= (a_start)))

inline void print_time(const char *name) {
    uint32_t time_low;
    uint32_t time_high;
    asm volatile(
        "cpuid\n" // Using CPUID instruction to serialize instruction execution
        "rdtsc"
        : "=a"(time_low), "=d"(time_high)
        :
        : "ebx", "ecx"
    );

    auto time = (size_t)time_low | (size_t)time_high << 32;

    printf("Time at %s: %zu\n", name, time);
}

struct MCFGTable {
    ACPI_TABLE_MCFG preamble;

    ACPI_MCFG_ALLOCATION allocations[0];
};

struct MADTTable {
    ACPI_TABLE_MADT preamble;

    ACPI_SUBTABLE_HEADER entries[0];
};

struct __attribute__((packed)) TSSEntry {
    uint32_t reserved_1;
    uint64_t rsp0;
    uint64_t rsp1;
    uint64_t rsp2;
    uint64_t reserved_2;
    uint64_t ist1;
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved_3;
    uint16_t reserved_4;
    uint16_t iopb_offset;
};

extern uint8_t stack_top[];

TSSEntry tss_entry {
    0,
    (uint64_t)&stack_top,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    0,
    (uint16_t)sizeof(TSSEntry)
};

struct __attribute__((packed)) GDTEntry {
    uint16_t limit_low;
    uint32_t base_low: 24;
    bool accessed: 1;
    bool read_write: 1;
    bool direction_conforming: 1;
    bool executable: 1;
    bool type: 1;
    uint8_t privilege: 2;
    bool present: 1;
    uint8_t limit_high: 4;
    bool task_available: 1;
    bool long_mode: 1;
    bool size: 1;
    bool granularity: 1;
    uint8_t base_high;
};

struct __attribute__((packed)) GDTDescriptor {
    uint16_t limit;
    uint32_t base;
};

const size_t gdt_size = 7;

GDTEntry gdt_entries[gdt_size] {
    {},
    { // Kernel code segment
        0xFFFF,
        0,
        false,
        true,
        false,
        true,
        true,
        0,
        true,
        0b1111,
        false,
        true,
        false,
        true,
        0,
    },
    { // Kernel data segment
        0,
        0,
        false,
        true,
        false,
        false,
        true,
        0,
        true,
        0,
        false,
        false,
        false,
        false,
        0,
    },
    { // User data segment
        0,
        0,
        false,
        true,
        false,
        false,
        true,
        3,
        true,
        0,
        false,
        false,
        false,
        false,
        0,
    },
    { // User code segment
        0xFFFF,
        0,
        false,
        true,
        false,
        true,
        true,
        3,
        true,
        0b1111,
        false,
        true,
        false,
        true,
        0,
    },
    { // Task State Segment
        (uint16_t)sizeof(TSSEntry),
        (uint32_t)(size_t)&tss_entry,
        true,
        false,
        false,
        true,
        false,
        3,
        true,
        (uint8_t)(sizeof(TSSEntry) >> 16),
        false,
        false,
        false,
        false,
        (uint8_t)((size_t)&tss_entry >> 24),
    }
};

GDTDescriptor gdt_descriptor {
    (uint16_t)(gdt_size * sizeof(GDTEntry) - 1),
    (uint32_t)(size_t)&gdt_entries
};

struct __attribute__((packed)) IDTEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t interrupt_stack_table: 3;
    uint8_t ignored: 5;
    uint8_t type: 4;
    bool storage_segment: 1;
    uint8_t privilege_level: 2;
    bool present: 1;
    uint64_t offset_high: 48;
    uint32_t reserved;
};

struct __attribute__((packed)) IDTDescriptor {
    uint16_t limit;
    uint64_t base;
};

volatile uint32_t *apic_registers;

extern "C" [[noreturn]] void  preempt_timer_thunk();

extern "C" [[noreturn]] void user_enter_thunk(const ProcessStackFrame *frame);

const uint32_t preempt_time = 0x100000;

using ProcessBucket = BucketArray<Process, 4>;

ProcessBucket processes {};

ProcessBucket::Iterator current_process_iterator;

const size_t kernel_memory_start = 0;
const size_t kernel_memory_end = 0x800000;

const auto kernel_pages_start = kernel_memory_start / page_size;
const auto kernel_pages_end = divide_round_up(kernel_memory_end, page_size);

const auto kernel_pd_start = kernel_pages_start / page_table_length;
const auto kernel_pd_end = divide_round_up(kernel_pages_end, page_table_length);
const auto kernel_pd_count = kernel_pd_end - kernel_pd_start;

const auto kernel_pdp_start = kernel_pd_start / page_table_length;
const auto kernel_pdp_end = divide_round_up(kernel_pd_end, page_table_length);
const auto kernel_pdp_count = kernel_pdp_end - kernel_pdp_start;

const auto kernel_pml4_start = kernel_pdp_start / page_table_length;
const auto kernel_pml4_end = divide_round_up(kernel_pdp_end, page_table_length);
const auto kernel_pml4_count = kernel_pml4_end - kernel_pml4_start;

__attribute__((aligned(page_size)))
PageTableEntry kernel_pml4_table[page_table_length] {};

__attribute__((aligned(page_size)))
PageTableEntry kernel_pdp_tables[kernel_pml4_count][page_table_length] {};

__attribute__((aligned(page_size)))
PageTableEntry kernel_pd_tables[kernel_pdp_count][page_table_length] {};

__attribute__((aligned(page_size)))
PageTableEntry kernel_page_tables[kernel_pd_count][page_table_length] {};

[[noreturn]] inline void halt() {
    printf("Halting...\n");

    while(true) {
        asm volatile("hlt");
    }
}

[[noreturn]] static void enter_next_process() {
    ++current_process_iterator;

    if(current_process_iterator.current_bucket == nullptr) {
        current_process_iterator = begin(processes);
    }

    if(current_process_iterator.current_bucket == nullptr) {
        printf("No processes left\n");

        halt();
    }

    auto process = *current_process_iterator;

    auto stack_frame_copy = process->frame;

    // Set timer value
    apic_registers[0x380 / 4] = preempt_time;

    // Send the End of Interrupt signal
    apic_registers[0x0B0 / 4] = 0;

    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(process->pml4_table_physical_address)
    );

    user_enter_thunk(&stack_frame_copy);
}

uint8_t *global_bitmap_entries;
size_t global_bitmap_size;

static void destroy_process(ProcessBucket::Iterator iterator) {
    iterator.current_bucket->occupied[iterator.current_sub_index] = false;
}

extern "C" [[noreturn]] void exception_handler(size_t index, const InterruptStackFrame *frame, size_t error_code) {
    printf("EXCEPTION 0x%X(0x%X) AT %p", index, error_code, frame->instruction_pointer);

    if(
        (size_t)frame->instruction_pointer < kernel_memory_start ||
        (size_t)frame->instruction_pointer >= kernel_memory_end
    ) {
        asm volatile(
            "mov %0, %%cr3"
            :
            : "D"(&kernel_pml4_table)
        );

        auto process = *current_process_iterator;

        printf(" in process %zu\n", process->id);

        destroy_process(current_process_iterator);

        enter_next_process();
    } else {
        printf(" in kernel\n");

        halt();
    }
}

extern "C" [[noreturn]] void preempt_timer_handler(const ProcessStackFrame *frame) {
    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_pml4_table)
    );

    auto old_process = *current_process_iterator;

    old_process->frame = *frame;

    enter_next_process();
}

__attribute((interrupt))
static void local_apic_spurious_interrupt(const InterruptStackFrame *interrupt_frame) {
    printf("Spurious interrupt at %p\n", interrupt_frame->instruction_pointer);
}

extern "C" void exception_handler_thunk_0();
extern "C" void exception_handler_thunk_1();
extern "C" void exception_handler_thunk_2();
extern "C" void exception_handler_thunk_3();
extern "C" void exception_handler_thunk_4();
extern "C" void exception_handler_thunk_5();
extern "C" void exception_handler_thunk_6();
extern "C" void exception_handler_thunk_7();
extern "C" void exception_handler_thunk_8();
extern "C" void exception_handler_thunk_10();
extern "C" void exception_handler_thunk_11();
extern "C" void exception_handler_thunk_12();
extern "C" void exception_handler_thunk_13();
extern "C" void exception_handler_thunk_14();
extern "C" void exception_handler_thunk_15();
extern "C" void exception_handler_thunk_16();
extern "C" void exception_handler_thunk_17();
extern "C" void exception_handler_thunk_18();
extern "C" void exception_handler_thunk_19();
extern "C" void exception_handler_thunk_20();
extern "C" void exception_handler_thunk_30();

const size_t idt_length = 48;

#define idt_entry_exception(index) {\
    (uint16_t)(size_t)&exception_handler_thunk_##index, \
    0x08, \
    0, \
    0, \
    0xF, \
    0, \
    0, \
    1, \
    (uint64_t)(size_t)&exception_handler_thunk_##index >> 16, \
    0 \
}

IDTEntry idt_entries[idt_length] {
    idt_entry_exception(0),
    idt_entry_exception(1),
    idt_entry_exception(2),
    idt_entry_exception(3),
    idt_entry_exception(4),
    idt_entry_exception(5),
    idt_entry_exception(6),
    idt_entry_exception(7),
    idt_entry_exception(8),
    {},
    idt_entry_exception(10),
    idt_entry_exception(11),
    idt_entry_exception(12),
    idt_entry_exception(13),
    idt_entry_exception(14),
    idt_entry_exception(15),
    idt_entry_exception(16),
    idt_entry_exception(17),
    idt_entry_exception(18),
    idt_entry_exception(19),
    idt_entry_exception(20),
    {}, {}, {}, {}, {}, {}, {}, {}, {},
    idt_entry_exception(30),
    {},
    {
       (uint16_t)(size_t)&preempt_timer_thunk,
        0x08,
        0,
        0,
        0xE,
        0,
        0,
        1,
        (uint64_t)(size_t)&preempt_timer_thunk >> 16,
        0 
    },
    {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {}, {},
    {
       (uint16_t)(size_t)&local_apic_spurious_interrupt,
        0x08,
        0,
        0,
        0xE,
        0,
        0,
        1,
        (uint64_t)(size_t)&local_apic_spurious_interrupt >> 16,
        0 
    }
};

IDTDescriptor idt_descriptor { idt_length * sizeof(IDTEntry) - 1, (uint64_t)&idt_entries };

extern uint8_t embedded_init_binary[];

size_t next_process_id = 0;

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

static bool create_process_from_elf(
    uint8_t *elf_binary,
    uint8_t *bitmap_entries,
    size_t bitmap_size,
    Process **result_processs,
    ProcessBucket::Iterator *result_process_iterator
) {
    auto process_iterator = find_unoccupied_bucket_slot(&processes);

    if(process_iterator.current_bucket == nullptr) {
        auto new_bucket = (ProcessBucket::Bucket*)map_and_allocate_memory(sizeof(ProcessBucket::Bucket), bitmap_entries, bitmap_size);
        if(new_bucket == nullptr) {
            return false;
        }

        memset((void*)new_bucket, 0, sizeof(ProcessBucket::Bucket));

        {
            auto current_bucket = &processes.first_bucket;
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
        return false;
    }

    process->logical_pages_start = process_user_pages_start;
    process->page_count = process_page_count;

    size_t process_kernel_pages_start;
    if(!find_free_logical_pages(process_page_count, &process_kernel_pages_start)) {
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
            return false;
        }

        // The order of these is IMPORTANT, otherwise the kernel pages will get overwritten!!!

        if(!set_page(process_user_pages_start + j, physical_page_index, process->pml4_table_physical_address, bitmap_entries, bitmap_size)) {
            return false;
        }

        if(!set_page(process_kernel_pages_start + j, physical_page_index, bitmap_entries, bitmap_size)) {
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
        return false;
    }

    unmap_pages(process_kernel_pages_start, process_page_count);

    for(size_t j = 0; j < process_stack_page_count; j += 1) {
        size_t physical_page_index;
        if(!allocate_next_physical_page(
            &bitmap_index,
            &bitmap_sub_bit_index,
            bitmap_entries,
            bitmap_size,
            &physical_page_index
        )){
            return false;
        }

        if(!set_page(process_stack_pages_start + j, physical_page_index, process->pml4_table_physical_address, bitmap_entries, bitmap_size)) {
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

extern "C" void syscall_thunk();

#define bits_to_mask(bits) ((1 << (bits)) - 1)

extern "C" void syscall_entrance(ProcessStackFrame *stack_frame) {
    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_pml4_table)
    );

    auto process = *current_process_iterator;

    auto syscall_index = stack_frame->rbx;
    auto parameter = stack_frame->rdx;

    auto return_1 = &stack_frame->rbx;
    auto return_2 = &stack_frame->rdx;

    static_assert(configuration_area_size == page_size, "PCI-E MMIO area not page-sized");

    switch((SyscallType)syscall_index) {
        case SyscallType::Exit: {
            destroy_process(current_process_iterator);

            enter_next_process();
        } break;

        case SyscallType::RelinquishTime: {
            process->frame = *stack_frame;

            enter_next_process();
        } break;

        case SyscallType::DebugPrint: {
            putchar((char)parameter);
        } break;

        case SyscallType::MapFreeMemory: {
            *return_1 = 0;

            auto page_count = divide_round_up(parameter, page_size);

            size_t logical_pages_start;
            if(!find_free_logical_pages(
                parameter,
                process->pml4_table_physical_address,
                global_bitmap_entries,
                global_bitmap_size,
                &logical_pages_start
            )) {
                break;
            }

            size_t bitmap_index = 0;
            size_t bitmap_sub_bit_index = 0;

            auto success = true;
            for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
                size_t physical_page_index;
                if(!allocate_next_physical_page(
                    &bitmap_index,
                    &bitmap_sub_bit_index,
                    global_bitmap_entries,
                    global_bitmap_size,
                    &physical_page_index
                )) {
                    success = false;
                    break;
                }

                if(!set_page(
                    logical_pages_start + relative_page_index,
                    physical_page_index,
                    process->pml4_table_physical_address,
                    global_bitmap_entries,
                    global_bitmap_size
                )) {
                    success = false;
                    break;
                }
            }

            if(success) {
                *return_1 = logical_pages_start * page_size;
            }
        } break;

        case SyscallType::MapFreeConsecutiveMemory: {
            *return_1 = 0;

            auto page_count = divide_round_up(parameter, page_size);

            size_t logical_pages_start;
            if(!find_free_logical_pages(
                page_count,
                process->pml4_table_physical_address,
                global_bitmap_entries,
                global_bitmap_size,
                &logical_pages_start
            )) {
                break;
            }

            size_t physical_pages_start;
            if(!allocate_consecutive_physical_pages(
                page_count,
                global_bitmap_entries,
                global_bitmap_size,
                &physical_pages_start
            )) {
                break;
            }

            auto success = true;
            for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
                if(!set_page(
                    logical_pages_start + relative_page_index,
                    physical_pages_start + relative_page_index,
                    process->pml4_table_physical_address,
                    global_bitmap_entries,
                    global_bitmap_size
                )) {
                    success = false;
                    break;
                }
            }

            if(!success) {
                break;
            }

            *return_1 = logical_pages_start * page_size;
            *return_2 = physical_pages_start * page_size;
        } break;

        case SyscallType::FindPCIEDevice: {
            auto desired_device_id = (uint16_t)parameter;
            auto desired_vendor_id = (uint16_t)(parameter >> 16);

            MCFGTable *mcfg_table;
            {
                auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);
                if(status != AE_OK) {
                    break;
                }
            }

            *return_1 = 0;

            auto done = false;
            for(size_t i = 0; i < (mcfg_table->preamble.Header.Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION); i += 1) {
                auto physical_memory_start = (size_t)mcfg_table->allocations[i].Address;
                auto segment = (size_t)mcfg_table->allocations[i].PciSegment;
                auto start_bus_number = (size_t)mcfg_table->allocations[i].StartBusNumber;
                auto end_bus_number = (size_t)mcfg_table->allocations[i].EndBusNumber;
                auto bus_count = end_bus_number - start_bus_number + 1;

                for(size_t bus = 0; bus < bus_count; bus += 1) {
                    auto bus_memory_size = device_count * function_count * configuration_area_size;

                    auto bus_memory = map_memory(
                        physical_memory_start + bus * device_count * function_count * configuration_area_size,
                        bus_memory_size,
                        global_bitmap_entries,
                        global_bitmap_size
                    );
                    if(bus_memory == nullptr) {
                        done = true;
                        break;
                    }

                    for(size_t device = 0; device < device_count; device += 1) {
                        for(size_t function = 0; function < function_count; function += 1) {
                            auto device_base_index = 
                                device * function_count * configuration_area_size +
                                function * configuration_area_size;

                            auto header = (volatile PCIHeader*)((size_t)bus_memory + device_base_index);

                            if(header->vendor_id == 0xFFFF) {
                                continue;
                            }

                            if(header->device_id == desired_device_id && header->vendor_id == desired_vendor_id) {
                                *return_1 = 1;
                                *return_2 =
                                    function |
                                    device << function_bits |
                                    bus << (function_bits + device_bits) |
                                    segment << (function_bits + device_bits + bus_bits);

                                done = true;
                                break;
                            }
                        }

                        if(done) {
                            break;
                        }
                    }

                    unmap_memory((void*)bus_memory, bus_memory_size);

                    if(done) {
                        break;
                    }
                }

                if(done) {
                    break;
                }
            }

            AcpiPutTable(&mcfg_table->preamble.Header);
        } break;

        case SyscallType::MapPCIEConfiguration: {
            auto function = parameter & bits_to_mask(function_bits);
            auto device = parameter >> function_bits & bits_to_mask(device_bits);
            auto bus = parameter >> (function_bits + device_bits) & bits_to_mask(bus_bits);
            auto target_segment = parameter >> (function_bits + device_bits + bus_bits);

            MCFGTable *mcfg_table;
            {
                auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);
                if(status != AE_OK) {
                    break;
                }
            }

            *return_1 = 0;

            for(size_t i = 0; i < (mcfg_table->preamble.Header.Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION); i += 1) {
                auto physical_memory_start = (size_t)mcfg_table->allocations[i].Address;
                auto segment = (size_t)mcfg_table->allocations[i].PciSegment;
                auto start_bus_number = (size_t)mcfg_table->allocations[i].StartBusNumber;

                if(segment == target_segment) {
                    size_t logical_pages_start;
                    if(!find_free_logical_pages(
                        1,
                        process->pml4_table_physical_address,
                        global_bitmap_entries,
                        global_bitmap_size,
                        &logical_pages_start
                    )) {
                        break;
                    }

                    if(!set_page(
                        logical_pages_start,
                        physical_memory_start / page_size +
                            bus * device_count * function_count +
                            device * function_count +
                            function,
                        process->pml4_table_physical_address,
                        global_bitmap_entries,
                        global_bitmap_size
                    )) {
                        break;
                    }

                    *return_1 = logical_pages_start * page_size;
                    break;
                }
            }

            AcpiPutTable(&mcfg_table->preamble.Header);
        } break;

        case SyscallType::MapPCIEBar: {
            auto bar_index = parameter & bits_to_mask(bar_index_bits);
            auto function = parameter >> bar_index_bits & bits_to_mask(function_bits);
            auto device = parameter >> (bar_index_bits + function_bits) & bits_to_mask(device_bits);
            auto bus = parameter >> (bar_index_bits + function_bits + device_bits) & bits_to_mask(bus_bits);
            auto target_segment = parameter >> (bar_index_bits + function_bits + device_bits + bus_bits);

            MCFGTable *mcfg_table;
            {
                auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);
                if(status != AE_OK) {
                    break;
                }
            }

            *return_1 = 0;

            for(size_t i = 0; i < (mcfg_table->preamble.Header.Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION); i += 1) {
                auto physical_memory_start = (size_t)mcfg_table->allocations[i].Address;
                auto segment = (size_t)mcfg_table->allocations[i].PciSegment;
                auto start_bus_number = (size_t)mcfg_table->allocations[i].StartBusNumber;

                if(segment == target_segment) {
                    auto configuration_memory = map_memory(
                        physical_memory_start +
                            (bus * device_count * function_count +
                            device * function_count +
                            function) * configuration_area_size,
                        configuration_area_size,
                        global_bitmap_entries,
                        global_bitmap_size
                    );
                    if(configuration_memory == nullptr) {
                        break;
                    }

                    auto header = (volatile PCIHeader*)configuration_memory;

                    auto bar_value = header->bars[bar_index];

                    auto bar_type = bar_value & bits_to_mask(bar_type_bits);

                    size_t address;
                    size_t size;
                    auto valid_bar = true;
                    if(bar_type == 0) { // Memory BAR
                        auto memory_bar_type = bar_value >> bar_type_bits & bits_to_mask(memory_bar_type_bits);

                        const auto info_bits = bar_type_bits + memory_bar_type_bits + memory_bar_prefetchable_bits;

                        switch(memory_bar_type) {
                            case 0b00: { // 32-bit memory BAR
                                address = (size_t)(bar_value & ~bits_to_mask(info_bits));

                                header->bars[bar_index] = -1;

                                auto temp_bar_value = header->bars[bar_index] & ~bits_to_mask(info_bits);

                                temp_bar_value = ~temp_bar_value;
                                temp_bar_value += 1;

                                size = (size_t)temp_bar_value;

                                header->bars[bar_index] = bar_value;
                            } break;

                            case 0b10: { // 64-bit memory BAR
                                auto second_bar_value = header->bars[bar_index + 1];

                                address = (size_t)(bar_value & ~bits_to_mask(info_bits)) | (size_t)second_bar_value << 32;

                                header->bars[bar_index] = -1;
                                header->bars[bar_index + 1] = -1;

                                size = (header->bars[bar_index] & ~bits_to_mask(info_bits)) | (size_t)header->bars[bar_index + 1] << 32;

                                size = ~size;
                                size += 1;

                                header->bars[bar_index] = bar_value;
                                header->bars[bar_index + 1] = second_bar_value;
                            } break;

                            default: {
                                valid_bar = false;
                            } break;
                        }
                    } else { // IO BAR
                        valid_bar = false;
                    }

                    if(!valid_bar) {
                        break;
                    }

                    unmap_memory(configuration_memory, configuration_area_size);

                    auto physical_pages_start = address / page_size;
                    auto physical_pages_end = divide_round_up(address + size, page_size);
                    auto page_count = physical_pages_end - physical_pages_start;

                    size_t logical_pages_start;
                    if(!find_free_logical_pages(
                        page_count,
                        process->pml4_table_physical_address,
                        global_bitmap_entries,
                        global_bitmap_size,
                        &logical_pages_start
                    )) {
                        break;
                    }

                    auto failed = false;
                    for(size_t relative_page_index = 0; relative_page_index < page_count; relative_page_index += 1) {
                        if(!set_page(
                            logical_pages_start + relative_page_index,
                            physical_pages_start + relative_page_index,
                            process->pml4_table_physical_address,
                            global_bitmap_entries,
                            global_bitmap_size
                        )) {
                            failed = true;
                            break;
                        }
                    }

                    if(failed) {
                        break;
                    }

                    *return_1 = logical_pages_start * page_size;
                }
            }

            AcpiPutTable(&mcfg_table->preamble.Header);
        } break;

        default: { // unknown syscall
            printf("Unknown syscall from process %zu at %p\n", process->id, stack_frame->interrupt_frame.instruction_pointer);

            destroy_process(current_process_iterator);

            enter_next_process();
        } break;
    }

    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(process->pml4_table_physical_address)
    );
}

extern void (*init_array_start[])();
extern void (*init_array_end[])();

struct BootstrapMemoryMapEntry {
    void* address;
    size_t length;
    bool available;
};

static bool find_free_physical_pages_in_bootstrap(
    const BootstrapMemoryMapEntry *bootstrap_map,
    size_t bootstrap_map_size,
    size_t page_count,
    size_t extra_pages_start,
    size_t extra_page_count,
    size_t *pages_start
) {
    for(size_t i = 0; i < bootstrap_map_size; i += 1) {
        auto entry = &bootstrap_map[i];
        if(entry->available) {
            auto start = (size_t)entry->address;
            auto end = start + entry->length;

            auto entry_pages_start = divide_round_up(start, page_size);
            auto entry_pages_end = end / page_size;
            auto entry_page_count = entry_pages_end - entry_pages_start;

            if(entry_page_count >= page_count) {
                for(size_t test_pages_start = entry_pages_start; test_pages_start < entry_pages_start - page_count; test_pages_start += 1) {
                    if(
                        !do_ranges_intersect(test_pages_start, test_pages_start + page_count, kernel_pages_start, kernel_pages_end) &&
                        !do_ranges_intersect(test_pages_start, test_pages_start + page_count, extra_pages_start, extra_pages_start + extra_page_count)
                    ) {
                        *pages_start = test_pages_start;
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

extern "C" void main(const BootstrapMemoryMapEntry *bootstrap_memory_map, size_t bootstrap_memory_map_size) {
    // Enable sse
    asm volatile(
        "mov %%cr0, %%rax\n"
        "or $(1 << 1), %%rax\n"
        "and $~(1 << 2), %%rax\n"
        "mov %%rax, %%cr0\n"
        "mov %%cr4, %%rax\n"
        "or $(1 << 9), %%rax\n"
        "or $(1 << 10), %%rax\n"
        "mov %%rax, %%cr4"
        :
        :
        : "rax"
    );

    auto initializer_count = ((size_t)init_array_end - (size_t)init_array_start) / sizeof(void (*)());

    for(size_t i = 0; i < initializer_count; i += 1) {
        init_array_start[i]();
    }

    clear_console();

    asm volatile(
        // Load GDT
        "lgdt (%0)\n"
        // Load data segments
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        // Load code segment
        "sub $16, %%rsp\n"
        "movq $.long_jump_target, (%%rsp)\n"
        "movq $0x08, 8(%%rsp)\n"
        "lretq\n"
        ".long_jump_target:\n"
        // Load task state register
        "mov $0x2B, %%ax\n"
        "ltr %%ax"
        :
        : "D"(&gdt_descriptor)
        : "ax"
    );

    asm volatile(
        // Load IDT
        "lidt (%0)\n"
        // Disable PIC
        "mov $0xff, %%al\n"
        "out %%al, $0xA1\n"
        "out %%al, $0x21\n"
        // Enable interupts
        "sti"
        :
        : "D"(&idt_descriptor)
        : "al"
    );

    size_t next_pdp_table_index = 0;
    size_t next_pd_table_index = 0;
    size_t next_page_table_index = 0;

    for(size_t total_page_index = kernel_pages_start; total_page_index < kernel_pages_end; total_page_index += 1) {
        auto page_index = total_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        PageTableEntry *pdp_table;
        if(kernel_pml4_table[pml4_index].present) {
            pdp_table = (PageTableEntry*)(kernel_pml4_table[pml4_index].page_address * page_size);
        } else {
            pdp_table = &kernel_pdp_tables[next_pdp_table_index][0];
            next_pdp_table_index += 1;

            kernel_pml4_table[pml4_index].present = true;
            kernel_pml4_table[pml4_index].write_allowed = true;
            kernel_pml4_table[pml4_index].user_mode_allowed = true;
            kernel_pml4_table[pml4_index].page_address = (size_t)pdp_table / page_size;

            next_pdp_table_index += 1;
        }

        PageTableEntry *pd_table;
        if(pdp_table[pdp_index].present) {
            pd_table = (PageTableEntry*)(pdp_table[pdp_index].page_address * page_size);
        } else {
            pd_table = &kernel_pd_tables[next_pd_table_index][0];
            next_pd_table_index += 1;

            pdp_table[pdp_index].present = true;
            pdp_table[pdp_index].write_allowed = true;
            pdp_table[pdp_index].user_mode_allowed = true;
            pdp_table[pdp_index].page_address = (size_t)pd_table / page_size;
        }

        PageTableEntry *page_table;
        if(pd_table[pd_index].present) {
            page_table = (PageTableEntry*)(pd_table[pd_index].page_address * page_size);
        } else {
            page_table = &kernel_page_tables[next_page_table_index][0];
            next_page_table_index += 1;

            pd_table[pd_index].present = true;
            pd_table[pd_index].write_allowed = true;
            pd_table[pd_index].user_mode_allowed = true;
            pd_table[pd_index].page_address = (size_t)page_table / page_size;
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = total_page_index;
    }

    kernel_pml4_table[page_table_length - 1].present = true;
    kernel_pml4_table[page_table_length - 1].write_allowed = true;
    kernel_pml4_table[page_table_length - 1].page_address = (size_t)kernel_pml4_table / page_size;

    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_pml4_table)
    );

    size_t highest_available_memory_end = 0;
    for(size_t i = 0; i < bootstrap_memory_map_size; i += 1) {
        auto entry = &bootstrap_memory_map[i];

        if(entry->available) {
            auto end = (size_t)entry->address + entry->length;

            if(end > highest_available_memory_end) {        
                highest_available_memory_end = end;
            }
        }
    }

    auto highest_available_pages_end = divide_round_up(highest_available_memory_end, page_size);

    auto bitmap_size = highest_available_pages_end / 8;
    auto bitmap_page_count = divide_round_up(bitmap_size, page_size);

    size_t bitmap_physical_pages_start;
    if(!find_free_physical_pages_in_bootstrap(
        bootstrap_memory_map,
        bootstrap_memory_map_size,
        bitmap_page_count,
        0,
        0,
        &bitmap_physical_pages_start
    )) {
        printf("Error: Out of bootstrap memory\n");

        halt();
    }

    auto new_page_table_count = count_page_tables_needed_for_logical_pages(kernel_pages_end, bitmap_page_count);

    size_t new_physical_pages_start;
    if(!find_free_physical_pages_in_bootstrap(
        bootstrap_memory_map,
        bootstrap_memory_map_size,
        new_page_table_count,
        bitmap_physical_pages_start,
        bitmap_page_count,
        &new_physical_pages_start
    )) {
        printf("Error: Out of bootstrap memory\n");

        halt();
    }

    size_t next_new_physical_page_index = 0;

    for(size_t relative_page_index = 0; relative_page_index < bitmap_page_count; relative_page_index += 1) {
        auto page_index = kernel_pages_end + relative_page_index;
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
            auto physical_pages_start = new_physical_pages_start + next_new_physical_page_index;
            next_new_physical_page_index += 1;

            pml4_table[pml4_index].present = true;
            pml4_table[pml4_index].write_allowed = true;
            pml4_table[pml4_index].user_mode_allowed = true;
            pml4_table[pml4_index].page_address = physical_pages_start;

            asm volatile(
                "invlpg (%0)"
                :
                : "D"(pdp_table)
            );

            memset((void*)pdp_table, 0, sizeof(PageTableEntry[page_table_length]));
        }

        auto pd_table = get_pd_table_pointer(pml4_index, pdp_index);

        if(!pdp_table[pdp_index].present) {
            auto physical_pages_start = new_physical_pages_start + next_new_physical_page_index;
            next_new_physical_page_index += 1;

            pdp_table[pdp_index].present = true;
            pdp_table[pdp_index].write_allowed = true;
            pdp_table[pdp_index].user_mode_allowed = true;
            pdp_table[pdp_index].page_address = physical_pages_start;

            asm volatile(
                "invlpg (%0)"
                :
                : "D"(pd_table)
            );

            memset((void*)pd_table, 0, sizeof(PageTableEntry[page_table_length]));
        }

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

        if(!pd_table[pd_index].present) {
            auto physical_pages_start = new_physical_pages_start + next_new_physical_page_index;
            next_new_physical_page_index += 1;

            pd_table[pd_index].present = true;
            pd_table[pd_index].write_allowed = true;
            pd_table[pd_index].user_mode_allowed = true;
            pd_table[pd_index].page_address = physical_pages_start;

            asm volatile(
                "invlpg (%0)"
                :
                : "D"(page_table)
            );

            memset((void*)page_table, 0, sizeof(PageTableEntry[page_table_length]));
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = bitmap_physical_pages_start + relative_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((kernel_pages_end + relative_page_index) * page_size)
        );
    }

    auto bitmap_entries = (uint8_t*)(kernel_pages_end * page_size);

    memset((void*)bitmap_entries, 0xFF, bitmap_size);

    for(size_t i = 0; i < bootstrap_memory_map_size; i += 1) {
        auto entry = &bootstrap_memory_map[i];

        if(entry->available) {
            auto start = (size_t)entry->address;
            auto end = start + entry->length;

            auto entry_pages_start = divide_round_up(start, page_size);
            auto entry_pages_end = end / page_size;
            auto entry_page_count = entry_pages_end - entry_pages_start;

            deallocate_bitmap_range(bitmap_entries, entry_pages_start, entry_page_count);
        }
    }

    allocate_bitmap_range(bitmap_entries, kernel_pages_start, kernel_pages_end - kernel_pages_start);

    allocate_bitmap_range(bitmap_entries, bitmap_physical_pages_start, bitmap_page_count);

    allocate_bitmap_range(bitmap_entries, new_physical_pages_start, new_page_table_count);

    global_bitmap_entries = bitmap_entries;
    global_bitmap_size = bitmap_size;

    AcpiInitializeSubsystem();

    AcpiInitializeTables(nullptr, 8, TRUE);

    MADTTable *madt_table;
    {
        auto status = AcpiGetTable((char*)ACPI_SIG_MADT, 1, (ACPI_TABLE_HEADER**)&madt_table);
        if(status != AE_OK) {
            printf("Error: Unable to get MADT ACPI table (0x%X)\n", status);

            halt();
        }
    }

    auto apic_physical_address = (size_t)madt_table->preamble.Address;

    size_t current_madt_index = 0;
    while(current_madt_index < madt_table->preamble.Header.Length - sizeof(ACPI_TABLE_MADT)) {
        auto header = *(ACPI_SUBTABLE_HEADER*)((size_t)madt_table->entries + current_madt_index);

        if(header.Type == ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE) {
            auto subtable = *(ACPI_MADT_LOCAL_APIC_OVERRIDE*)((size_t)madt_table->entries + current_madt_index + sizeof(header));

            apic_physical_address = (size_t)subtable.Address;

            break;
        }

        current_madt_index += (size_t)header.Length;
    }

    AcpiPutTable(&madt_table->preamble.Header);

    apic_physical_address = 0xFEE00000;

    apic_registers = (volatile uint32_t*)map_memory(apic_physical_address, 0x400, bitmap_entries, bitmap_size);
    if(apic_registers == nullptr) {
        printf("Error: Out of memory\n");

        halt();
    }

    // Set APIC to known state
    apic_registers[0x2F0 / 4] |= 1 << 16;
    apic_registers[0x320 / 4] |= 1 << 16;
    apic_registers[0x330 / 4] |= 1 << 16;
    apic_registers[0x340 / 4] |= 1 << 16;
    apic_registers[0x350 / 4] |= 1 << 16;
    apic_registers[0x360 / 4] |= 1 << 16;
    apic_registers[0x370 / 4] |= 1 << 16;

    // Globally enable APICs
    asm volatile(
        "mov $0x1B, %%ecx\n" // IA32_APIC_BASE MSR
        "rdmsr\n"
        "or $(1 << 11), %%eax\n"
        "wrmsr\n"
        :
        :
        : "edx"
    );

    // Enable local APIC / set spurious interrupt vector to 0x2F
    apic_registers[0xF0 / 4] = 0x2F | 0x100;

    // Enable timer / set timer interrupt vector vector
    apic_registers[0x320 / 4] = 0x20;

    // Set timer divider to 16
    apic_registers[0x3E0 / 4] = 3;

    // Set up syscall/sysret instructions

    asm volatile(
        "mov $0xC0000080, %%ecx\n" // IA32_EFER MSR
        "rdmsr\n"
        "or $1, %%eax\n"
        "wrmsr\n"
        :
        :
        : "edx"
    );

    asm volatile(
        "wrmsr"
        :
        : "a"((uint32_t)-1), "d"((uint32_t)-1), "c"((uint32_t)0xC0000084) // IA32_FMASK MSR
    );

    asm volatile(
        "wrmsr"
        :
        : "a"((uint32_t)(size_t)syscall_thunk), "d"((uint32_t)((size_t)syscall_thunk >> 32)), "c"((uint32_t)0xC0000082) // IA32_LSTAR MSR
    );

    asm volatile(
        "wrmsr"
        :
        : "a"((uint32_t)0), "d"((uint32_t)0x10 << 16 | (uint32_t)0x08), "c"((uint32_t)0xC0000081) // IA32_STAR MSR
        // sysretq adds 16 (wtf why?) to the selector to get the code selector so 0x10 ( + 16 = 0x20) is used above...
    );

    printf("Loading init process...\n");

    Process *process;
    ProcessBucket::Iterator process_iterator;
    if(!create_process_from_elf(embedded_init_binary, bitmap_entries, bitmap_size, &process, &process_iterator)) {
        printf("Error: Out of memory\n");

        halt();
    }

    current_process_iterator = begin(processes);

    auto stack_frame_copy = process->frame;

    // Set timer value
    apic_registers[0x380 / 4] = preempt_time;

    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(process->pml4_table_physical_address)
    );

    user_enter_thunk(&stack_frame_copy);
}

void *allocate(size_t size) {
    auto base_pointer = map_and_allocate_memory(page_size + size, global_bitmap_entries, global_bitmap_size);

    if(base_pointer == nullptr) {
        return nullptr;
    }

    *(size_t*)base_pointer = size;

    return (void*)((size_t)base_pointer + page_size);
}

void deallocate(void *pointer) {
    auto base_pointer = (void*)((size_t)pointer - page_size);

    auto size = *(size_t*)base_pointer;

    unmap_and_deallocate_memory(base_pointer, page_size + size, global_bitmap_entries, global_bitmap_size);
}

extern "C" void *memset(void *destination, int value, size_t count) {
    auto temp_destination = destination;

    asm volatile(
        "rep stosb"
        : "=D"(temp_destination), "=c"(count)
        : "D"(temp_destination), "a"((uint8_t)value), "c"(count)
    );

    return destination;
}

extern "C" void *memcpy(void *destination, const void *source, size_t count) {
    auto temp_destination = destination;

    asm volatile(
        "rep movsb"
        : "=S"(source), "=D"(temp_destination), "=c"(count)
        : "S"(source), "D"(temp_destination), "c"(count)
    );

    return destination;
}