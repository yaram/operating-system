#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "acpi.h"
}
#include "console.h"
#include "heap.h"
#include "paging.h"

size_t next_heap_index = 0;
const size_t heap_size = 1024 * 1024;
uint8_t heap[heap_size];

struct MCFGTable {
    ACPI_TABLE_MCFG preamble;

    ACPI_MCFG_ALLOCATION allocations[0];
};

extern "C" void main() {
    clear_console();

    const size_t inital_pages_start = 0;
    const size_t inital_pages_length = 0x800000;

    if(!create_initial_pages(inital_pages_start / page_size, inital_pages_length / page_size)) {
        printf("Error: Unable to allocate initial pages\n");

        return;
    }

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

    printf("Finished enumerating PCI-E devices\n");
}

void *allocate(size_t size) {
    auto index = next_heap_index;

    next_heap_index += size;

    if(next_heap_index > heap_size) {
        return nullptr;
    }

    return (void*)(index + (size_t)heap);
}

void deallocate(void *pointer) {

}