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
#include "halt.h"
#include "array.h"

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

Processes::Iterator current_process_iterator;

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

extern uint8_t embedded_init_binary[];

Array<uint8_t> global_bitmap;

Processes global_processes {};

[[noreturn]] static void enter_next_process() {
    ++current_process_iterator;

    if(current_process_iterator.current_bucket == nullptr) {
        current_process_iterator = begin(global_processes);
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

extern "C" [[noreturn]] void exception_handler(size_t index, const ProcessStackFrame *frame) {
    printf("EXCEPTION 0x%X(0x%X) AT %p", index, frame->interrupt_frame.error_code, frame->interrupt_frame.instruction_pointer);

    if(
        (size_t)frame->interrupt_frame.instruction_pointer < kernel_memory_start ||
        (size_t)frame->interrupt_frame.instruction_pointer >= kernel_memory_end
    ) {
        asm volatile(
            "mov %0, %%cr3"
            :
            : "D"(&kernel_pml4_table)
        );

        auto process = *current_process_iterator;

        printf(" in process %zu\n", process->id);

        destroy_process(current_process_iterator, global_bitmap);

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

extern "C" void syscall_thunk();

#define bits_to_mask(bits) ((1 << (bits)) - 1)

inline void clear_pages(size_t page_index, size_t page_count) {
    memset((void*)(page_index * page_size), 0, page_count * page_size);
}

enum struct MapProcessMemoryResult {
    Success,
    OutOfMemory,
    InvalidMemoryRange
};

static MapProcessMemoryResult map_process_memory_into_kernel(Process *process, size_t user_memory_start, size_t size, void **kernel_memory_start) {
    auto user_memory_end = user_memory_start + size;

    auto user_pages_start = user_memory_start / page_size;
    auto user_pages_end = divide_round_up(user_memory_end, page_size);
    auto page_count = user_pages_end - user_pages_start;

    auto offset = user_memory_start - user_pages_start * page_size;

    auto found = false;
    for(auto iterator = begin(process->mappings); iterator != end(process->mappings); ++iterator) {
        auto mapping = *iterator;

        if(
            user_pages_start >= mapping->logical_pages_start &&
            user_pages_end <= mapping->logical_pages_start + mapping->page_count
        ) {
            found = true;
            break;
        }
    }

    if(!found) {
        return MapProcessMemoryResult::InvalidMemoryRange;
    }

    size_t kernel_pages_start;
    if(!map_pages_from_user(
        user_pages_start,
        page_count,
        process->pml4_table_physical_address,
        global_bitmap,
        &kernel_pages_start
    )) {
        return MapProcessMemoryResult::OutOfMemory;
    }

    *kernel_memory_start = (void*)(kernel_pages_start * page_size + offset);
    return MapProcessMemoryResult::Success;
}

extern "C" void syscall_entrance(ProcessStackFrame *stack_frame) {
    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_pml4_table)
    );

    auto process = *current_process_iterator;

    auto syscall_index = stack_frame->rbx;
    auto parameter_1 = stack_frame->rdx;
    auto parameter_2 = stack_frame->rsi;

    auto return_1 = &stack_frame->rbx;
    auto return_2 = &stack_frame->rdx;

    static_assert(configuration_area_size == page_size, "PCI-E MMIO area not page-sized");

    switch((SyscallType)syscall_index) {
        case SyscallType::Exit: {
            destroy_process(current_process_iterator, global_bitmap);

            enter_next_process();
        } break;

        case SyscallType::RelinquishTime: {
            process->frame = *stack_frame;

            enter_next_process();
        } break;

        case SyscallType::DebugPrint: {
            putchar((char)parameter_1);
        } break;

        case SyscallType::MapFreeMemory: {
            *return_1 = 0;

            auto page_count = divide_round_up(parameter_1, page_size);

            size_t kernel_pages_start;
            if(!map_and_allocate_pages(page_count, global_bitmap, &kernel_pages_start)) {
                break;
            }

            size_t user_pages_start;
            if(!map_pages_from_kernel(
                kernel_pages_start,
                page_count,
                UserPermissions::Write,
                process->pml4_table_physical_address,
                global_bitmap,
                &user_pages_start
            )) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                break;
            }

            if(!register_process_mapping(process, user_pages_start, page_count, false, true, global_bitmap)) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                unmap_pages(user_pages_start, page_count, process->pml4_table_physical_address, false, global_bitmap);

                break;
            }

            clear_pages(kernel_pages_start, page_count);

            unmap_pages(kernel_pages_start, page_count);

            *return_1 = user_pages_start * page_size;
        } break;

        case SyscallType::MapFreeConsecutiveMemory: {
            *return_1 = 0;

            auto page_count = divide_round_up(parameter_1, page_size);

            size_t physical_pages_start;
            if(!allocate_consecutive_physical_pages(
                page_count,
                global_bitmap,
                &physical_pages_start
            )) {
                break;
            }

            size_t kernel_pages_start;
            if(!map_pages(physical_pages_start, page_count, global_bitmap, &kernel_pages_start)) {
                break;
            }

            size_t user_pages_start;
            if(!map_pages(
                physical_pages_start,
                page_count,
                UserPermissions::Write,
                process->pml4_table_physical_address,
                global_bitmap,
                &user_pages_start
            )) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                break;
            }

            if(!register_process_mapping(process, user_pages_start, page_count, false, true, global_bitmap)) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                unmap_pages(user_pages_start, page_count, process->pml4_table_physical_address, false, global_bitmap);

                break;
            }

            clear_pages(kernel_pages_start, page_count);

            unmap_pages(kernel_pages_start, page_count);

            *return_1 = user_pages_start * page_size;
            *return_2 = physical_pages_start * page_size;
        } break;

        case SyscallType::CreateSharedMemory: {
            *return_1 = 0;

            auto size = parameter_1;
            auto page_count = divide_round_up(size, page_size);

            size_t kernel_pages_start;
            if(!map_and_allocate_pages(page_count, global_bitmap, &kernel_pages_start)) {
                break;
            }

            size_t user_pages_start;
            if(!map_pages_from_kernel(
                kernel_pages_start,
                page_count,
                UserPermissions::Write,
                process->pml4_table_physical_address,
                global_bitmap,
                &user_pages_start
            )) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                break;
            }

            if(!register_process_mapping(process, user_pages_start, page_count, true, true, global_bitmap)) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                unmap_pages(user_pages_start, page_count, process->pml4_table_physical_address, false, global_bitmap);

                break;
            }

            clear_pages(kernel_pages_start, page_count);

            unmap_pages(kernel_pages_start, page_count);

            *return_1 = user_pages_start * page_size;
        } break;

        case SyscallType::MapSharedMemory: {
            const MapSharedMemoryParameters *parameters;
            switch(map_process_memory_into_kernel(process, parameter_1, sizeof(MapSharedMemoryParameters), (void**)&parameters)) {
                case MapProcessMemoryResult::Success: {
                    auto target_logical_pages_start = parameters->address / page_size;
                    auto target_logical_pages_end = divide_round_up(parameters->address + parameters->size, page_size);

                    auto page_count = target_logical_pages_end - target_logical_pages_start;

                    *return_1 = (size_t)MapSharedMemoryResult::InvalidProcessID;
                    for(auto target_process : global_processes) {
                        if(target_process->id == parameters->process_id) {
                            *return_1 = (size_t)MapSharedMemoryResult::InvalidProcessID;
                            for(auto mapping : target_process->mappings) {
                                if(
                                    mapping->logical_pages_start == target_logical_pages_start &&
                                    mapping->page_count == page_count &&
                                    mapping->is_shared
                                ) {
                                    auto page_count = mapping->page_count;

                                    size_t logical_pages_start;
                                    if(!map_pages_between_user(
                                        target_logical_pages_start,
                                        page_count,
                                        UserPermissions::Write,
                                        target_process->pml4_table_physical_address,
                                        process->pml4_table_physical_address,
                                        global_bitmap,
                                        &logical_pages_start
                                    )) {
                                        *return_1 = (size_t)MapSharedMemoryResult::OutOfMemory;
                                        break;
                                    }

                                    if(!register_process_mapping(process, logical_pages_start, page_count, true, false, global_bitmap)) {
                                        *return_1 = (size_t)MapSharedMemoryResult::OutOfMemory;

                                        break;
                                    }

                                    *return_1 = (size_t)MapSharedMemoryResult::Success;
                                    *return_2 = logical_pages_start * page_size;

                                    break;
                                }
                            }

                            break;
                        }
                    }

                    unmap_memory((void*)parameters, sizeof(MapSharedMemoryParameters));
                } break;

                case MapProcessMemoryResult::OutOfMemory: {
                    *return_1 = (size_t)MapSharedMemoryResult::OutOfMemory;
                } break;

                case MapProcessMemoryResult::InvalidMemoryRange: {
                    *return_1 = (size_t)MapSharedMemoryResult::InvalidMemoryRange;
                } break;
            }
        } break;

        case SyscallType::UnmapMemory: {
            auto logical_pages_start = parameter_1 / page_size;

            for(auto iterator = begin(process->mappings); iterator != end(process->mappings); ++iterator) {
                auto mapping = *iterator;

                if(mapping->logical_pages_start == logical_pages_start) {
                    iterator.current_bucket->occupied[iterator.current_sub_index] = false;

                    unmap_pages(
                        mapping->logical_pages_start,
                        mapping->page_count,
                        process->pml4_table_physical_address,
                        mapping->is_owned,
                        global_bitmap
                    );

                    break;
                }
            }
        } break;

        case SyscallType::CreateProcess: {
            auto elf_user_memory_start = parameter_1;
            auto elf_size = parameter_2;

            uint8_t *elf_binary;
            switch(map_process_memory_into_kernel(process,elf_user_memory_start, elf_size, (void**)&elf_binary)) {
                case MapProcessMemoryResult::Success: {
                    Process *new_process;
                    Processes::Iterator new_process_iterator;
                    switch(create_process_from_elf(
                        elf_binary,
                        global_bitmap,
                        &global_processes,
                        &new_process,
                        &new_process_iterator
                    )) {
                        case CreateProcessFromELFResult::Success: {
                            *return_1 = (size_t)CreateProcessResult::Success;
                            *return_2 = new_process->id;
                        } break;

                        case CreateProcessFromELFResult::OutOfMemory: {
                            *return_1 = (size_t)CreateProcessResult::OutOfMemory;
                        } break;

                        case CreateProcessFromELFResult::InvalidELF: {
                            *return_1 = (size_t)CreateProcessResult::InvalidELF;
                        } break;

                        default: halt();
                    }

                    unmap_memory(elf_binary, elf_size);
                } break;

                case MapProcessMemoryResult::OutOfMemory: {
                    *return_1 = (size_t)CreateProcessResult::OutOfMemory;
                } break;

                case MapProcessMemoryResult::InvalidMemoryRange: {
                    *return_1 = (size_t)CreateProcessResult::InvalidMemoryRange;
                } break;
            }
        } break;

        case SyscallType::FindPCIEDevice: {
            const FindPCIEDeviceParameters *parameters;
            switch(map_process_memory_into_kernel(process, parameter_1, sizeof(FindPCIEDeviceParameters), (void**)&parameters)) {
                case MapProcessMemoryResult::Success: {
                    MCFGTable *mcfg_table;
                    {
                        auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);
                        if(status != AE_OK) {
                            *return_1 = (size_t)FindPCIEDeviceResult::NotFound;

                            break;
                        }
                    }

                    *return_1 = (size_t)FindPCIEDeviceResult::NotFound;

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
                                global_bitmap
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

                                    if(!(
                                        (parameters->require_vendor_id && header->vendor_id != parameters->vendor_id) ||
                                        (parameters->require_device_id && header->device_id != parameters->device_id) ||
                                        (parameters->require_class_code && header->class_code != parameters->class_code) ||
                                        (parameters->require_subclass && header->subclass != parameters->subclass) ||
                                        (parameters->require_interface && header->interface != parameters->interface)
                                    )) {
                                        *return_1 = (size_t)FindPCIEDeviceResult::Success;
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

                    unmap_memory((void*)parameters, sizeof(FindPCIEDeviceParameters));       
                } break;

                case MapProcessMemoryResult::OutOfMemory: {
                    *return_1 = (size_t)FindPCIEDeviceResult::OutOfMemory;
                } break;

                case MapProcessMemoryResult::InvalidMemoryRange: {
                    *return_1 = (size_t)FindPCIEDeviceResult::InvalidMemoryRange;
                } break;
            }
        } break;

        case SyscallType::MapPCIEConfiguration: {
            auto function = parameter_1 & bits_to_mask(function_bits);
            auto device = parameter_1 >> function_bits & bits_to_mask(device_bits);
            auto bus = parameter_1 >> (function_bits + device_bits) & bits_to_mask(bus_bits);
            auto target_segment = parameter_1 >> (function_bits + device_bits + bus_bits);

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
                    if(!map_pages(
                        physical_memory_start / page_size +
                            bus * device_count * function_count +
                            device * function_count +
                            function,
                        1,
                        UserPermissions::Write,
                        process->pml4_table_physical_address,
                        global_bitmap,
                        &logical_pages_start
                    )) {
                        break;
                    }

                    if(!register_process_mapping(process, logical_pages_start, 1, false, false, global_bitmap)) {
                        break;
                    }

                    *return_1 = logical_pages_start * page_size;

                    break;
                }
            }

            AcpiPutTable(&mcfg_table->preamble.Header);
        } break;

        case SyscallType::MapPCIEBar: {
            auto bar_index = parameter_1 & bits_to_mask(bar_index_bits);
            auto function = parameter_1 >> bar_index_bits & bits_to_mask(function_bits);
            auto device = parameter_1 >> (bar_index_bits + function_bits) & bits_to_mask(device_bits);
            auto bus = parameter_1 >> (bar_index_bits + function_bits + device_bits) & bits_to_mask(bus_bits);
            auto target_segment = parameter_1 >> (bar_index_bits + function_bits + device_bits + bus_bits);

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
                        global_bitmap
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
                    if(!map_pages(
                        physical_pages_start,
                        page_count,
                        UserPermissions::Write,
                        process->pml4_table_physical_address,
                        global_bitmap,
                        &logical_pages_start
                    )) {
                        break;
                    }

                    if(!register_process_mapping(process, logical_pages_start, page_count, false, false, global_bitmap)) {
                        break;
                    }

                    *return_1 = logical_pages_start * page_size;
                }
            }

            AcpiPutTable(&mcfg_table->preamble.Header);
        } break;

        default: { // unknown syscall
            printf("Unknown syscall from process %zu at %p\n", process->id, stack_frame->interrupt_frame.instruction_pointer);

            destroy_process(current_process_iterator, global_bitmap);

            enter_next_process();
        } break;
    }

    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(process->pml4_table_physical_address)
    );
}

struct BootstrapMemoryMapEntry {
    void* address;
    size_t length;
    bool available;
};

static bool find_free_physical_pages_in_bootstrap(
    ConstArray<BootstrapMemoryMapEntry> bootstrap_memory_map,
    size_t page_count,
    size_t extra_pages_start,
    size_t extra_page_count,
    size_t *pages_start
) {
    for(size_t i = 0; i < bootstrap_memory_map.length; i += 1) {
        auto entry = &bootstrap_memory_map[i];
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

extern void (*init_array_start[])();
extern void (*init_array_end[])();

extern "C" void main(const BootstrapMemoryMapEntry *bootstrap_memory_map_entries, size_t bootstrap_memory_map_length) {
    ConstArray<BootstrapMemoryMapEntry> bootstrap_memory_map {
        bootstrap_memory_map_entries,
        bootstrap_memory_map_length
    };

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
    for(size_t i = 0; i < bootstrap_memory_map.length; i += 1) {
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

    // Enable execution-disable page bit
    asm volatile(
        "mov $0xC0000080, %%ecx\n" // IA32_EFER MSR
        "rdmsr\n"
        "or $(1 << 11), %%eax\n"
        "wrmsr\n"
        :
        :
        : "edx"
    );

    Array<uint8_t> bitmap {
        (uint8_t*)(kernel_pages_end * page_size),
        bitmap_page_count * page_size
    };

    memset((void*)bitmap.data, 0xFF, bitmap_size);

    for(size_t i = 0; i < bootstrap_memory_map.length; i += 1) {
        auto entry = &bootstrap_memory_map[i];

        if(entry->available) {
            auto start = (size_t)entry->address;
            auto end = start + entry->length;

            auto entry_pages_start = divide_round_up(start, page_size);
            auto entry_pages_end = end / page_size;
            auto entry_page_count = entry_pages_end - entry_pages_start;

            deallocate_bitmap_range(bitmap, entry_pages_start, entry_page_count);
        }
    }

    allocate_bitmap_range(bitmap, kernel_pages_start, kernel_pages_end - kernel_pages_start);

    allocate_bitmap_range(bitmap, bitmap_physical_pages_start, bitmap_page_count);

    allocate_bitmap_range(bitmap, new_physical_pages_start, new_page_table_count);

    global_bitmap = bitmap;

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

    apic_registers = (volatile uint32_t*)map_memory(apic_physical_address, 0x400, bitmap);
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

    Process *init_process;
    switch(create_process_from_elf(embedded_init_binary, bitmap, &global_processes, &init_process, &current_process_iterator)) {
        case CreateProcessFromELFResult::Success: break;

        case CreateProcessFromELFResult::OutOfMemory: {
            printf("Error: Out of memory\n");

            halt();
        } break;

        case CreateProcessFromELFResult::InvalidELF: {
            printf("Error: Init process ELF file is invalid\n");

            halt();
        } break;

        default: halt();
    }

    // Set ABI-specified intial register states

    init_process->frame.mxcsr |= bits_to_mask(6) << 7;

    auto stack_frame_copy = init_process->frame;

    // Set timer value
    apic_registers[0x380 / 4] = preempt_time;

    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(init_process->pml4_table_physical_address)
    );

    user_enter_thunk(&stack_frame_copy);
}

void *allocate(size_t size) {
    auto base_pointer = map_and_allocate_memory(page_size + size, global_bitmap);

    if(base_pointer == nullptr) {
        return nullptr;
    }

    *(size_t*)base_pointer = size;

    return (void*)((size_t)base_pointer + page_size);
}

void deallocate(void *pointer) {
    auto base_pointer = (void*)((size_t)pointer - page_size);

    auto size = *(size_t*)base_pointer;

    unmap_and_deallocate_memory(base_pointer, page_size + size, global_bitmap);
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