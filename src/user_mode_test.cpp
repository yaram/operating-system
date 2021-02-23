#include <stdint.h>
#include <stddef.h>
#include "printf.h"
#include "syscalls.h"
#include "pcie.h"

#define bits_to_mask(bits) ((1 << (bits)) - 1)

inline size_t syscall(SyscallType syscall_type, size_t parameter, size_t *return_2) {
    size_t return_1;
    asm volatile(
        "syscall"
        : "=b"(return_1), "=d"(*return_2)
        : "b"(syscall_type), "d"(parameter)
        : "rax", "rcx", "r11"
    );

    return return_1;
}

inline void syscall(SyscallType syscall_type, size_t parameter) {
    size_t return_2;
    syscall(syscall_type, parameter, &return_2);
}

void _putchar(char character) {
    syscall(SyscallType::DebugPrint, character);
}

struct __attribute__((packed)) virtio_pci_cap {
    uint8_t cap_vndr;
    uint8_t cap_next;
    uint8_t cap_len;
    uint8_t cfg_type;
    uint8_t bar;
    uint8_t padding[3];
    uint32_t offset;
    uint32_t length;
};

struct __attribute__((packed)) virtio_pci_common_cfg {
    uint32_t device_feature_select;
    uint32_t device_feature;
    uint32_t driver_feature_select;
    uint32_t driver_feature;
    uint16_t msix_config;
    uint16_t num_queues;
    uint8_t device_status;
    uint8_t config_generation;

    uint16_t queue_select;
    uint16_t queue_size;
    uint16_t queue_msix_vector;
    uint16_t queue_enable;
    uint16_t queue_notify_off;
    uint64_t queue_desc;
    uint64_t queue_driver;
    uint64_t queue_device;
};

extern "C" void entry() {
    const uint16_t virtio_gpu_vendor_id = 0x1Af4;
    const uint16_t virtio_gpu_device_id = 0x1050;

    size_t virtio_gpu_location;
    if(syscall(
        SyscallType::FindPCIEDevice,
        (size_t)virtio_gpu_device_id | (size_t)virtio_gpu_vendor_id << 16,
        &virtio_gpu_location
    ) == 0) {
        printf("Error: virtio-gpu device not present\n");

        syscall(SyscallType::Exit, 0);
    }

    size_t virtio_gpu_configuration_address;
    if(syscall(SyscallType::MapPCIEConfiguration, virtio_gpu_location, &virtio_gpu_configuration_address) == 0) {
        printf("Error: Unable to map virtio-gpu configuration space\n");

        syscall(SyscallType::Exit, 0);
    }

    auto configuration_header = (volatile PCIHeader*)virtio_gpu_configuration_address;

    size_t common_bar_index;
    size_t common_bar_offset;
    auto found = false;
    auto current_capability = (volatile virtio_pci_cap*)(virtio_gpu_configuration_address + configuration_header->capabilities_offset);
    if(current_capability->cap_len != 0) {
        while(true) {
            if(current_capability->cfg_type == 1) {
                common_bar_index = current_capability->bar;
                common_bar_offset = current_capability->offset;

                found = true;
                break;
            }

            if(current_capability->cap_next != 0) {
                current_capability = (volatile virtio_pci_cap*)(virtio_gpu_configuration_address + current_capability->cap_next);
            } else {
                break;
            }
        }
    }

    if(!found) {
        printf("Error: virtio-gpu common configuration capability not found\n");

        syscall(SyscallType::Exit, 0);
    }

    size_t common_bar_address;
    if(syscall(SyscallType::MapPCIEBar, common_bar_index | virtio_gpu_location << bar_index_bits, &common_bar_address) == 0) {
        printf("Error: virtio-gpu common configuration capability not found\n");

        syscall(SyscallType::Exit, 0);
    }

    auto common_configuration = (volatile virtio_pci_common_cfg*)(common_bar_address + common_bar_offset);

    printf("Test: %zX\n", common_configuration->device_status);

    syscall(SyscallType::Exit, 0);
}