#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "acpi.h"
}
#include "console.h"
#include "heap.h"
#include "paging.h"
#include "memory_map.h"
#include "elfload.h"
#include "process.h"

extern "C" void *memset(void *destination, int value, size_t count);
extern "C" void *memcpy(void *destination, const void *source, size_t count);

struct MCFGTable {
    ACPI_TABLE_MCFG preamble;

    ACPI_MCFG_ALLOCATION allocations[0];
};

struct MADTTable {
    ACPI_TABLE_MADT preamble;

    ACPI_SUBTABLE_HEADER entries[0];
};

const MemoryMapEntry *global_memory_map;
size_t global_memory_map_size;

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

extern "C" [[noreturn]] void exception_handler(size_t index, const InterruptStackFrame *frame, size_t error_code) {
    printf("EXCEPTION 0x%X(0x%X) AT %p\n", index, error_code, frame->instruction_pointer);

    while(true) {
        __asm volatile("hlt");
    }
}

volatile uint32_t *apic_registers;

extern "C" [[noreturn]] void  preempt_timer_thunk();

extern "C" [[noreturn]] void user_enter_thunk(const ProcessStackFrame *frame);

Process processes[process_count] {};

size_t current_process_index = 0;

const uint32_t preempt_time = 0x100000;

PageTables kernel_tables {};

extern "C" [[noreturn]] void preempt_timer_handler(const ProcessStackFrame *frame) {
    __asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_tables)
    );

    processes[current_process_index].frame = *frame;

    current_process_index += 1;
    current_process_index %= process_count;

    auto stack_frame = &processes[current_process_index].frame;

    // Set timer value
    apic_registers[0x380 / 4] = preempt_time;

    // Send the End of Interrupt signal
    apic_registers[0x0B0 / 4] = 0;

    __asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&processes[current_process_index].tables.pml4_table)
    );

    user_enter_thunk(stack_frame);
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

extern uint8_t user_mode_test[];

static bool elf_read(el_ctx *context, void *destination, size_t length, size_t offset) {
    memcpy(destination, &user_mode_test[offset], length);

    return true;
}

static void *elf_allocate(el_ctx *context, Elf_Addr physical_memory_start, Elf_Addr virtual_memory_start, Elf_Addr size) {
    return (void*)physical_memory_start;
}

extern "C" void syscall_thunk();

extern "C" void syscall_entrance(const ProcessStackFrame *stack_frame) {
    __asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_tables)
    );

    printf("Syscall at %p from process %zu\n", stack_frame->interrupt_frame.instruction_pointer, current_process_index);

    __asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&processes[current_process_index].tables.pml4_table)
    );
}

extern void (*init_array_start[])();
extern void (*init_array_end[])();

extern "C" void main(const MemoryMapEntry *memory_map, size_t memory_map_size) {
    global_memory_map = memory_map;
    global_memory_map_size = memory_map_size;

    auto initializer_count = ((size_t)init_array_end - (size_t)init_array_start) / sizeof(void (*)());

    for(size_t i = 0; i < initializer_count; i += 1) {
        init_array_start[i]();
    }

    clear_console();

    __asm volatile(
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

    __asm volatile(
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

    for(size_t i = 0; i < memory_map_size; i += 1) {
        printf("Memory region 0x%zX, 0x%zX, %d\n", (size_t)memory_map[i].address, memory_map[i].length, memory_map[i].available);
    }

    auto kernel_pages_start = kernel_memory_start / page_size;
    auto kernel_pages_end = kernel_memory_end / page_size;
    auto kernel_pages_count = kernel_pages_end - kernel_pages_start + 1;

    if(!map_consecutive_pages(&kernel_tables, kernel_pages_start, kernel_pages_start, kernel_pages_count, false, false)) {
        printf("Error: Unable to map initial pages in kernel space\n");

        return;
    }

    __asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_tables.pml4_table)
    );

    AcpiInitializeSubsystem();

    AcpiInitializeTables(nullptr, 8, FALSE);

    MCFGTable *mcfg_table;
    {
        auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);
        if(status != AE_OK) {
            printf("Error: Unable to get MCFG ACPI table (0x%X)\n", status);

            return;
        }
    }

    const size_t device_count = 32;
    const size_t function_count = 8;

    const size_t function_area_size = 4096;

    for(size_t i = 0; i < (mcfg_table->preamble.Header.Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION); i += 1) {
        auto physical_memory_start = (size_t)mcfg_table->allocations[i].Address;
        auto segment = (size_t)mcfg_table->allocations[i].PciSegment;
        auto start_bus_number = (size_t)mcfg_table->allocations[i].StartBusNumber;
        auto end_bus_number = (size_t)mcfg_table->allocations[i].EndBusNumber;
        auto bus_count = end_bus_number - start_bus_number + 1;

        for(size_t bus = 0; bus < bus_count; bus += 1) {
            auto bus_memory_size = device_count * function_count * function_area_size;

            auto bus_memory = (uint8_t*)map_memory(
                &kernel_tables,
                physical_memory_start + bus * device_count * function_count * function_area_size,
                bus_memory_size,
                false,
                true
            );
            if(bus_memory == nullptr) {
                printf(
                    "Error: Unable to map pages for PCI-E bus %zu on segment %u at 0x%0zx(%zx)\n",
                    bus + start_bus_number,
                    segment,
                    physical_memory_start,
                    bus_memory_size
                );

                return;
            }

            for(size_t device = 0; device < device_count; device += 1) {
                for(size_t function = 0; function < function_count; function += 1) {
                    auto device_base_index = 
                        device * function_count * function_area_size +
                        function * function_area_size;

                    auto vendor_id = *(uint16_t*)&bus_memory[device_base_index + 0];

                    if(vendor_id == 0xFFFF) {
                        continue;
                    }

                    auto device_id = *(uint16_t*)&bus_memory[device_base_index + 2];

                    printf("PCI-E device 0x%4.X:0x%4.X at %d/%d/%d\n", vendor_id, device_id, bus + start_bus_number, device, function);
                }
            }

            unmap_memory(&kernel_tables, (void*)bus_memory, bus_memory_size, true);
        }
    }

    AcpiPutTable(&mcfg_table->preamble.Header);

    MADTTable *madt_table;
    {
        auto status = AcpiGetTable((char*)ACPI_SIG_MADT, 1, (ACPI_TABLE_HEADER**)&madt_table);
        if(status != AE_OK) {
            printf("Error: Unable to get MADT ACPI table (0x%X)\n", status);

            return;
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

    apic_registers = (volatile uint32_t*)map_memory(&kernel_tables, apic_physical_address, 0x400, false, true);
    if(apic_registers == nullptr) {
        printf("Error: Unable to map memory for APIC registers\n");

        return;
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
    __asm volatile(
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

    __asm volatile(
        "mov $0xC0000080, %%ecx\n" // IA32_EFER MSR
        "rdmsr\n"
        "or $1, %%eax\n"
        "wrmsr\n"
        :
        :
        : "edx"
    );

    __asm volatile(
        "wrmsr"
        :
        : "a"((uint32_t)-1), "d"((uint32_t)-1), "c"((uint32_t)0xC0000084) // IA32_FMASK MSR
    );

    __asm volatile(
        "wrmsr"
        :
        : "a"((uint32_t)(size_t)syscall_thunk), "d"((uint32_t)((size_t)syscall_thunk >> 32)), "c"((uint32_t)0xC0000082) // IA32_LSTAR MSR
    );

    __asm volatile(
        "wrmsr"
        :
        : "a"((uint32_t)0), "d"((uint32_t)0x10 << 16 | (uint32_t)0x08), "c"((uint32_t)0xC0000081) // IA32_STAR MSR
        // sysretq adds 16 (wtf why?) to the selector to get the code selector so 0x10 ( + 16 = 0x20) is used above...
    );

    printf("Loading user mode processes...\n");

    for(size_t i = 0; i < process_count; i += 1) {
        el_ctx elf_context {};
        elf_context.pread = &elf_read;

        {
            auto status = el_init(&elf_context);
            if(status != EL_OK) {
                printf("Error: Unable to initialize elfloader (%d)\n", status);

                return;
            }
        }

        if(!map_consecutive_pages(&processes[i].tables, kernel_pages_start, kernel_pages_start, kernel_pages_count, false, false)) {
            printf("Error: Unable to map initial pages in user space for process %zu\n", i);

            return;
        }

        size_t process_physical_memory_start;
        if(!find_unoccupied_physical_memory(elf_context.memsz, &kernel_tables, processes, memory_map, memory_map_size, &process_physical_memory_start)) {
            printf("Error: Unable to find unoccupied memory of size %zu for process %zu\n", elf_context.memsz, i);

            return;
        }

        auto kernel_mapped_process_memory = map_memory(&kernel_tables, process_physical_memory_start, elf_context.memsz, false, true);
        if(kernel_mapped_process_memory == nullptr) {
            printf("Error: Unable to map kernel memory of size %zu for process %zu\n", elf_context.memsz, i);

            return;
        }

        auto user_mapped_process_memory = map_memory(&processes[i].tables, process_physical_memory_start, elf_context.memsz, true, false);
        if(user_mapped_process_memory == nullptr) {
            printf("Error: Unable to map user memory of size %zu for process %zu\n", elf_context.memsz, i);

            return;
        }

        {
            auto status = el_init(&elf_context);
            if(status != EL_OK) {
                printf("Error: Unable to initialize elfloader (%d)\n", status);

                return;
            }
        }

        elf_context.base_load_paddr = (Elf64_Addr)kernel_mapped_process_memory;
        elf_context.base_load_vaddr = (Elf64_Addr)user_mapped_process_memory;

        {
            auto status = el_load(&elf_context, &elf_allocate);
            if(status != EL_OK) {
                printf("Error: Unable to load ELF binary (%d)\n", status);

                return;
            }
        }

        {
            auto status = el_relocate(&elf_context);
            if(status != EL_OK) {
                printf("Error: Unable to perform ELF relocations (%d)\n", status);

                return;
            }
        }

        unmap_memory(&kernel_tables, kernel_mapped_process_memory, elf_context.memsz, true);

        const size_t process_stack_size = 4096;

        size_t stack_physical_memory_start;
        if(!find_unoccupied_physical_memory(process_stack_size, &kernel_tables, processes, memory_map, memory_map_size, &stack_physical_memory_start)) {
            printf("Error: Unable to find unoccupied memory of size %zu for process %zu stack\n", process_stack_size, i);

            return;
        }

        auto stack_bottom = map_memory(&processes[i].tables, stack_physical_memory_start, process_stack_size, true, false);
        if(stack_bottom == nullptr) {
            printf("Error: Unable to map process stack memory of size %zu for process %zu\n", process_stack_size, i);

            return;
        }

        auto stack_top = (void*)((size_t)stack_bottom + process_stack_size);

        auto entry_point = (void*)((size_t)user_mapped_process_memory + (size_t)elf_context.ehdr.e_entry);

        processes[i].frame.interrupt_frame.instruction_pointer = entry_point;
        processes[i].frame.interrupt_frame.code_segment = 0x23;
        processes[i].frame.interrupt_frame.cpu_flags = 1 << 9;
        processes[i].frame.interrupt_frame.stack_pointer = stack_top;
        processes[i].frame.interrupt_frame.stack_segment = 0x1B;
    }

    current_process_index = 0;

    // Set timer value
    apic_registers[0x380 / 4] = preempt_time;

    __asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&processes[0].tables.pml4_table)
    );

    user_enter_thunk(&processes[0].frame);
}

void *allocate(size_t size) {
    size_t physical_memory_start;
    if(!find_unoccupied_physical_memory(size, &kernel_tables, processes, global_memory_map, global_memory_map_size, &physical_memory_start)) {
        return nullptr;
    }

    return map_memory(&kernel_tables, physical_memory_start, size, false, true);
}

void deallocate(void *pointer) {

}

extern "C" void *memset(void *destination, int value, size_t count) {
    __asm volatile(
        "rep stosb"
        :
        : "D"(destination), "a"((uint8_t)value), "c"(count)
    );

    return destination;
}

extern "C" void *memcpy(void *destination, const void *source, size_t count) {
    __asm volatile(
        "rep movsb"
        :
        : "S"(source), "D"(destination), "c"(count)
    );

    return destination;
}