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

extern "C" void *memset(void *destination, int value, size_t count);
extern "C" void *memcpy(void *destination, const void *source, size_t count);

struct MCFGTable {
    ACPI_TABLE_MCFG preamble;

    ACPI_MCFG_ALLOCATION allocations[0];
};

MemoryMapEntry *global_memory_map;
size_t global_memory_map_size;

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
    uint8_t ignored: 1;
    bool long_mode: 1;
    bool size: 1;
    bool granularity: 1;
    uint8_t base_high;
};

struct __attribute__((packed)) GDTDescriptor {
    uint16_t limit;
    uint32_t base;
};

const size_t gdt_size = 5;

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
        0,
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
        0,
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
        0,
        true,
        false,
        true,
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
        0,
        false,
        false,
        false,
        0,
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

struct __attribute__((packed)) InterruptStackFrame {
    void *instruction_pointer;
    uint64_t code_segment;
    uint64_t cpu_flags;
    void *stack_pointer;
    uint64_t stack_segment;
};

static void exception_handler(size_t index, InterruptStackFrame frame, size_t error_code) {
    printf("EXCEPTION 0x%X(0x%X) AT %p\n", index, error_code, frame.instruction_pointer);

    while(true) {
        __asm volatile("hlt");
    }
}

#define exception_thunk(index) \
__attribute((interrupt)) static void exception_handler_thunk_##index(InterruptStackFrame *interrupt_frame) { \
    exception_handler(index, *interrupt_frame, 0); \
}

#define exception_thunk_error_code(index) \
__attribute((interrupt)) static void exception_handler_thunk_##index(InterruptStackFrame *interrupt_frame, size_t error_code) { \
    exception_handler(index, *interrupt_frame, error_code); \
}

exception_thunk(0);
exception_thunk(1);
exception_thunk(2);
exception_thunk(3);
exception_thunk(4);
exception_thunk(5);
exception_thunk(6);
exception_thunk(7);
exception_thunk_error_code(8);
exception_thunk_error_code(10);
exception_thunk_error_code(11);
exception_thunk_error_code(12);
exception_thunk_error_code(13);
exception_thunk_error_code(14);
exception_thunk(15);
exception_thunk(16);
exception_thunk_error_code(17);
exception_thunk(18);
exception_thunk(19);
exception_thunk(20);
exception_thunk_error_code(30);

const size_t idt_length = 32;

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
    {}
};

IDTDescriptor idt_descriptor { idt_length * sizeof(IDTEntry) - 1, (uint64_t)&idt_entries };

extern "C" void __cxx_global_var_init();

extern uint8_t user_mode_test[];

static bool elf_read(el_ctx *context, void *destination, size_t length, size_t offset) {
    memcpy(destination, &user_mode_test[offset], length);

    return true;
}

static void *elf_allocate(el_ctx *context, Elf_Addr physical_memory_start, Elf_Addr virtual_memory_start, Elf_Addr size) {
    return (void*)physical_memory_start;
}

extern "C" [[noreturn]] void user_enter_thunk(void *entry_address, void *stack_pointer);
extern "C" void syscall_thunk();

extern "C" void syscall_entrance(void *return_address, void *stack_pointer) {
    printf("Syscall at %p (%p)...\n", return_address, stack_pointer);
}

extern "C" void main(MemoryMapEntry *memory_map, size_t memory_map_size) {
    global_memory_map = memory_map;
    global_memory_map_size = memory_map_size;

    clear_console();

    __cxx_global_var_init();

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

    if(!create_initial_pages(kernel_memory_start / page_size, kernel_memory_end / page_size)) {
        printf("Error: Unable to allocate initial pages\n");

        return;
    }

    AcpiInitializeSubsystem();

    AcpiInitializeTables(nullptr, 8, FALSE);

    MCFGTable *mcfg_table;
    auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);

    if(status != AE_OK) {
        printf("Error: Unable to get MCFG ACPI table (0x%X)\n", status);

        return;
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
                physical_memory_start + bus * device_count * function_count * function_area_size,
                bus_memory_size,
                false
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

            unmap_memory((void*)bus_memory, bus_memory_size);
        }
    }

    // Set up syscall/sysret instructions

    __asm volatile(
        "mov $0xC0000080, %rcx\n" // IA32_EFER MSR
        "rdmsr\n"
        "or $1, %eax\n"
        "wrmsr\n"
    );

    __asm volatile(
        "wrmsr"
        :
        : "a"((uint32_t)0), "d"((uint32_t)0), "c"((uint32_t)0xC0000084) // IA32_FMASK MSR
    );

    __asm volatile(
        "wrmsr"
        :
        : "a"((uint32_t)(size_t)syscall_thunk), "d"((uint32_t)((size_t)syscall_thunk >> 32)), "c"((uint32_t)0xC0000082) // IA32_LSTAR MSR
    );

    __asm volatile(
        "wrmsr"
        :
        : "a"(0), "d"((size_t)0x18 | (size_t)0x08 << 16), "c"((uint32_t)0xC0000081) // IA32_STAR MSR
    );

    printf("Loading user mode ELF...\n");

    el_ctx elf_context {};
    elf_context.pread = &elf_read;

    {
        auto status = el_init(&elf_context);
        if(status != EL_OK) {
            printf("Error: Unable to initialize elfloader (%d)\n", status);

            return;
        }
    }

    auto user_mode_memory_start = map_any_memory(elf_context.memsz, true, memory_map, memory_map_size);
    if(user_mode_memory_start == nullptr) {
        printf("Error: Unable to map memory of size %zu for user mode\n", elf_context.memsz);

        return;
    }

    {
        auto status = el_init(&elf_context);
        if(status != EL_OK) {
            printf("Error: Unable to initialize elfloader (%d)\n", status);

            return;
        }
    }

    elf_context.base_load_paddr = (Elf64_Addr)user_mode_memory_start;
    elf_context.base_load_vaddr = (Elf64_Addr)user_mode_memory_start;

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

    const size_t user_mode_stack_size = 4096;

    auto user_mode_stack_bottom = map_any_memory(user_mode_stack_size, true, memory_map, memory_map_size);
    if(user_mode_stack_bottom == nullptr) {
        printf("Error: Unable to allocate user mode stack of size %uz\n", user_mode_stack_size);

        return;
    }

    auto user_mode_stack_top = (void*)((size_t)user_mode_stack_bottom + user_mode_stack_size);

    auto entry_point = (void*)((size_t)user_mode_memory_start + elf_context.ehdr.e_entry);

    printf("%p\n", entry_point);

    printf("Entering user mode...\n");

    user_enter_thunk(entry_point, user_mode_stack_top);
}

void *allocate(size_t size) {
    return map_any_memory(size, false, global_memory_map, global_memory_map_size);
}

void deallocate(void *pointer) {

}

extern "C" void *memset(void *destination, int value, size_t count) {
    __asm volatile(
        "rep stosb"
        :
        : "S"(destination), "a"((uint8_t)value), "c"(count)
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