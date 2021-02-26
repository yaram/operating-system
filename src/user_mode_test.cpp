#include <stdint.h>
#include <stddef.h>
#include "printf.h"
#include "syscalls.h"
#include "pcie.h"

#define bits_to_mask(bits) ((1 << (bits)) - 1)

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

inline size_t syscall(SyscallType syscall_type, size_t parameter) {
    size_t return_2;
    return syscall(syscall_type, parameter, &return_2);
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

struct virtio_pci_notify_cap : virtio_pci_cap { 
    uint32_t notify_off_multiplier;
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

struct __attribute__((packed)) virtq_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct virtq_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[];
};

struct virtq_used_elem {
    uint32_t id;
    uint32_t len;
};

struct virtq_used {
    uint16_t flags;
    uint16_t idx;
    virtq_used_elem ring[];
};

enum struct virtio_gpu_ctrl_type : uint32_t {
    /* 2d commands */
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D,
    VIRTIO_GPU_CMD_RESOURCE_UNREF,
    VIRTIO_GPU_CMD_SET_SCANOUT,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO,
    VIRTIO_GPU_CMD_GET_CAPSET,
    VIRTIO_GPU_CMD_GET_EDID,

    /* cursor commands */
    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR,

    /* success responses */
    VIRTIO_GPU_RESP_OK_NODATA = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO,
    VIRTIO_GPU_RESP_OK_CAPSET,
    VIRTIO_GPU_RESP_OK_EDID,

    /* error responses */
    VIRTIO_GPU_RESP_ERR_UNSPEC = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER,
};

struct virtio_gpu_ctrl_hdr { 
    virtio_gpu_ctrl_type type;
    uint32_t flags;
    uint64_t fence_id;
    uint32_t ctx_id;
    uint32_t padding;
};

struct virtio_gpu_rect {
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
};

const size_t VIRTIO_GPU_MAX_SCANOUTS = 16;

struct virtio_gpu_resp_display_info : virtio_gpu_ctrl_hdr {
    struct virtio_gpu_display_one {
        struct virtio_gpu_rect r;
        uint32_t enabled;
        uint32_t flags;
    } pmodes[VIRTIO_GPU_MAX_SCANOUTS];
};

[[noreturn]] inline void exit() {
    syscall(SyscallType::Exit, 0);

    while(true);
}

static volatile virtio_pci_cap *find_capability(size_t configuration_address, uint8_t type) {
    auto configuration_header = (volatile PCIHeader*)configuration_address;

    auto current_capability = (volatile virtio_pci_cap*)(configuration_address + configuration_header->capabilities_offset);
    if(current_capability->cap_len != 0) {
        while(true) {
            if(current_capability->cfg_type == type && (configuration_header->bars[current_capability->bar] & 1) == 0) {
                return current_capability;
            }

            if(current_capability->cap_next != 0) {
                current_capability = (volatile virtio_pci_cap*)(configuration_address + current_capability->cap_next);
            } else {
                break;
            }
        }
    }

    return nullptr;
}

extern "C" [[noreturn]] void entry() {
    const uint16_t virtio_gpu_vendor_id = 0x1Af4;
    const uint16_t virtio_gpu_device_id = 0x1050;

    size_t virtio_gpu_location;
    if(syscall(
        SyscallType::FindPCIEDevice,
        (size_t)virtio_gpu_device_id | (size_t)virtio_gpu_vendor_id << 16,
        &virtio_gpu_location
    ) == 0) {
        printf("Error: virtio-gpu device not present\n");

        exit();
    }

    auto virtio_gpu_configuration_address = syscall(SyscallType::MapPCIEConfiguration, virtio_gpu_location);
    if(virtio_gpu_configuration_address == 0) {
        printf("Error: Unable to map virtio-gpu configuration space\n");

        exit();
    }

    auto configuration_header = (volatile PCIHeader*)virtio_gpu_configuration_address;

    auto common_config_capability = find_capability(virtio_gpu_configuration_address, 1);
    if(common_config_capability == nullptr) {
        printf("Error: virtio-gpu common configuration capability not found\n");

        exit();
    }

    auto common_bar_address = syscall(SyscallType::MapPCIEBar, common_config_capability->bar | virtio_gpu_location << bar_index_bits);
    if(common_bar_address == 0) {
        printf("Error: Unable to map commmon config BAR for virtio-gpu\n");

        exit();
    }

    auto common_configuration = (volatile virtio_pci_common_cfg*)(common_bar_address + common_config_capability->offset);

    common_configuration->device_status = 0; // Reset virtio-gpu
    common_configuration->device_status |= 1; // Set ACKNOWLEDGE flag
    common_configuration->device_status |= 1 << 1; // Set DRIVER flag
    
    common_configuration->driver_feature_select = 0;
    common_configuration->device_feature = 0;

    common_configuration->device_feature_select = 1;
    common_configuration->device_feature = 1; // Set VIRTIO_F_VERSION_1 flag

    common_configuration->device_status |= 1 << 3; // Set FEATURES_OK flag

    if((common_configuration->device_status & ~(1 << 3)) == 0) { // Check for FEATURES_OK flag
        printf("Error: virtio-gpu device is an unsupported legacy device\n");

        exit();
    }

    common_configuration->queue_select = 0; // Select controlq queue

    const size_t queue_descriptor_count = 2;

    size_t queue_descriptors_physical_address;
    auto queue_descriptors = (volatile virtq_desc*)syscall(SyscallType::MapFreeConsecutiveMemory, sizeof(virtq_desc) * queue_descriptor_count, &queue_descriptors_physical_address);
    if(queue_descriptors == nullptr) {
        printf("Error: Unable to allocate memory for queue descriptor\n");

        exit();
    }

    common_configuration->queue_size = queue_descriptor_count; // Set controlq queue size

    queue_descriptors[0].flags = 0b1; // Set device readable (driver buffer), set has next
    queue_descriptors[0].next = 1; // Link to second descriptor

    queue_descriptors[1].flags = 0b10; // Set device writable (device buffer)

    const auto buffer_size = 1024;

    size_t buffers_physical_address;
    auto buffers_address = syscall(SyscallType::MapFreeConsecutiveMemory, buffer_size * queue_descriptor_count, &buffers_physical_address);
    if(buffers_address == 0) {
        printf("Error: Unable to allocate memory for queue buffers\n");

        exit();
    }

    for(size_t i = 0; i < queue_descriptor_count; i += 1) {
        queue_descriptors[i].addr = buffers_physical_address + buffer_size * i;
        queue_descriptors[i].len = buffer_size;
    }

    size_t available_ring_physical_address;
    auto available_ring = (volatile virtq_avail*)syscall(
        SyscallType::MapFreeConsecutiveMemory,
        sizeof(virtq_avail) + sizeof(uint16_t) * queue_descriptor_count,
        &available_ring_physical_address
    );
    if(available_ring == nullptr) {
        printf("Error: Unable to allocate memory for available ring\n");

        exit();
    }

    memset((void*)available_ring, 0, sizeof(virtq_avail) + sizeof(uint16_t) * queue_descriptor_count);

    size_t used_ring_physical_address;
    auto used_ring = (volatile virtq_used*)syscall(
        SyscallType::MapFreeConsecutiveMemory,
        sizeof(virtq_used) + sizeof(virtq_used_elem) * queue_descriptor_count,
        &used_ring_physical_address
    );
    if(used_ring == nullptr) {
        printf("Error: Unable to allocate memory for used ring\n");

        exit();
    }

    memset((void*)used_ring, 0, sizeof(virtq_used) + sizeof(virtq_used_elem) * queue_descriptor_count);

    common_configuration->queue_desc = queue_descriptors_physical_address;
    common_configuration->queue_driver = available_ring_physical_address;
    common_configuration->queue_device = used_ring_physical_address;

    common_configuration->queue_enable = 1;

    auto notify_capability = (volatile virtio_pci_notify_cap*)find_capability(virtio_gpu_configuration_address, 2);
    if(notify_capability == nullptr) {
        printf("Error: virtio-gpu notify capability not found\n");

        exit();
    }

    auto notify_bar_address = syscall(SyscallType::MapPCIEBar, notify_capability->bar | virtio_gpu_location << bar_index_bits);
    if(notify_bar_address == 0) {
        printf("Error: Unable to map notify BAR for virtio-gpu\n");

        exit();
    }

    auto notify = (volatile uint16_t*)(
        notify_bar_address +
        notify_capability->offset +
        common_configuration->queue_notify_off * notify_capability->notify_off_multiplier
    );

    common_configuration->device_status |= 1 << 2; // Set DRIVER_OK flag

    auto command_header = (volatile virtio_gpu_ctrl_hdr*)buffers_address;
    command_header->type = virtio_gpu_ctrl_type::VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    command_header->flags = 0;

    queue_descriptors[0].len = sizeof(virtio_gpu_ctrl_hdr);

    available_ring->ring[available_ring->idx % queue_descriptor_count] = 0;

    auto previous_used_index = used_ring->idx;

    available_ring->idx += 1;

    *notify = 0;

    while(used_ring->idx == previous_used_index);

    auto response = (volatile virtio_gpu_resp_display_info*)(buffers_address + buffer_size);

    if(response->type != virtio_gpu_ctrl_type::VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        printf("Error: Invalid response from VIRTIO_GPU_CMD_GET_DISPLAY_INFO command\n");

        exit();
    }

    printf("Display 0 size: %u by %u\n", response->pmodes[0].r.width, response->pmodes[0].r.height);

    exit();
}