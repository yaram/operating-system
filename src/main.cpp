#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "acpi.h"
}
#include "console.h"
#include "heap.h"
#include "paging.h"
#include "memory_map.h"

struct MCFGTable {
    ACPI_TABLE_MCFG preamble;

    ACPI_MCFG_ALLOCATION allocations[0];
};

MemoryMapEntry *global_memory_map;
size_t global_memory_map_size;

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
    0x18, \
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

extern "C" void main(MemoryMapEntry *memory_map, size_t memory_map_size) {
    global_memory_map = memory_map;
    global_memory_map_size = memory_map_size;

    clear_console();

    __cxx_global_var_init();

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

            auto bus_memory = (uint8_t*)map_memory(physical_memory_start + bus * device_count * function_count * function_area_size, bus_memory_size);
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
}

void *allocate(size_t size) {
    return map_any_memory(size, global_memory_map, global_memory_map_size);
}

void deallocate(void *pointer) {

}