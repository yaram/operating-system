#include <stdint.h>
#include "Uefi.h"
#include "Guid/Acpi.h"
#include "kernel_extern.h"
#include "printf.h"

#define divide_round_up(dividend, divisor) (((dividend) + (divisor) - 1) / (divisor))

extern "C" uint8_t kernel_binary[];
extern "C" uint8_t kernel_binary_end[];

static EFI_SYSTEM_TABLE *global_system_table;

void _putchar(char character) {
    CHAR16 buffer[2];
    buffer[0] = character;
    buffer[1] = '\0';

    global_system_table->ConOut->OutputString(global_system_table->ConOut, buffer);
}

const static size_t kernel_binary_load_address = 0x200000;

static_assert(bootstrap_space_address + sizeof(BootstrapSpace) <= kernel_binary_load_address, "Bootstrap space is too large");

#define efi_call(call, message) {auto status=(call);if(EFI_ERROR(status)){printf("ERROR: %s (0x%X)\r\n",(message),(UINT32)status);return status;}}

static inline bool compare_memory(const void *a, const void *b, size_t count) {
    bool result;
    asm volatile(
        "repe cmpsb\n"
        "sete %3"
        : "=S"(a), "=D"(b), "=c"(count), "=r"(result)
        : "S"(a), "D"(b), "c"(count)
    );

    return result;
}

#define do_ranges_intersect(a_start, a_end, b_start, b_end) (!((a_end) <= (b_start) || (b_end) <= (a_start)))

EFI_STATUS efi_main(EFI_HANDLE ImageHandle, EFI_SYSTEM_TABLE *SystemTable) {
    global_system_table = SystemTable;

    {
        auto address = (EFI_PHYSICAL_ADDRESS)bootstrap_space_address;
        efi_call(SystemTable->BootServices->AllocatePages(
            AllocateAddress,
            EfiLoaderData,
            divide_round_up(sizeof(BootstrapSpace), EFI_PAGE_SIZE),
            &address
        ), "Unable to map bootstrap space memory");
    }

    auto bootstrap_space = (BootstrapSpace*)bootstrap_space_address;

    EFI_GUID acpi_table_guid EFI_ACPI_TABLE_GUID;

    auto found_acpi_table = false;
    for(size_t i = 0; i < SystemTable->NumberOfTableEntries; i += 1) {
        if(compare_memory(&SystemTable->ConfigurationTable[i].VendorGuid, &acpi_table_guid, sizeof(EFI_GUID))) {
            bootstrap_space->acpi_table_physical_address = (size_t)SystemTable->ConfigurationTable[i].VendorTable;

            found_acpi_table = true;
            break;
        }
    }

    if(!found_acpi_table) {
        printf("ERROR: Unable to find ACPI table\r\n");

        return -1;
    }

    auto kernel_binary_size = (size_t)&kernel_binary_end - (size_t)&kernel_binary;

    {
        auto address = (EFI_PHYSICAL_ADDRESS)kernel_binary_load_address;
        efi_call(SystemTable->BootServices->AllocatePages(
            AllocateAddress,
            EfiLoaderData,
            divide_round_up(kernel_binary_size, EFI_PAGE_SIZE),
            &address
        ), "Unable to map kernel binary memory");
    }

    SystemTable->BootServices->CopyMem((void*)kernel_binary_load_address, &kernel_binary, kernel_binary_size);

    UINTN memory_map_size = 0;
    UINTN map_key;
    UINTN descriptor_size;
    UINT32 descriptor_version;
    SystemTable->BootServices->GetMemoryMap(
        &memory_map_size,
        nullptr,
        &map_key,
        &descriptor_size,
        &descriptor_version
    );

    // Extra size to make room for any memory mappings AllocatePool does below
    memory_map_size += 8 * descriptor_size;

    void *memory_map_buffer;
    efi_call(SystemTable->BootServices->AllocatePool(
        EfiLoaderData,
        memory_map_size,
        &memory_map_buffer
    ), "Unable to allocate UEFI memory map buffer");

    efi_call(SystemTable->BootServices->GetMemoryMap(
        &memory_map_size,
        (EFI_MEMORY_DESCRIPTOR*)memory_map_buffer,
        &map_key,
        &descriptor_size,
        &descriptor_version
    ), "Unable to retrieve UEFI memory map");

    size_t bootstrap_memory_map_index = 0;
    size_t memory_map_index = 0;
    while(memory_map_index < memory_map_size) {
        auto descriptor = (EFI_MEMORY_DESCRIPTOR*)((size_t)memory_map_buffer + memory_map_index);

        switch(descriptor->Type) {
            case EfiLoaderCode:
            case EfiLoaderData:
            case EfiBootServicesCode:
            case EfiBootServicesData:
            case EfiConventionalMemory:
            case EfiRuntimeServicesCode:
            case EfiRuntimeServicesData: {
                if(bootstrap_memory_map_index >= BootstrapSpace::memory_map_max_size) {
                    printf("ERROR: Bootstrap memory map too small\r\n");

                    return -1;
                }

                bootstrap_space->memory_map[bootstrap_memory_map_index] = {
                    descriptor->PhysicalStart,
                    descriptor->NumberOfPages * EFI_PAGE_SIZE,
                    true
                };
                bootstrap_memory_map_index += 1;
            } break;
        }

        memory_map_index += descriptor_size;
    }

    bootstrap_space->memory_map_size = bootstrap_memory_map_index;

    printf("Exiting boot services and entering kernel...\r\n");

    SystemTable->BootServices->ExitBootServices(ImageHandle, map_key);

    auto kernel_main = (KernelMain*)kernel_binary_load_address;

    auto initial_stack_top = (size_t)&bootstrap_space->stack + BootstrapSpace::stack_size;

    asm volatile(
        "mov %0, %%rsp\n"
        "call *%1"
        :
        : "r"(initial_stack_top), "r"(kernel_main), "D"((uint64_t)true)
    );

    // Shouldn't ever get to this point
    UNREACHABLE();
}

// To satisfy link.exe
extern "C" int _fltused = 0;