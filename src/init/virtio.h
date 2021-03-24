#pragma once

#include <stdint.h>
#include <stddef.h>
#include "pcie.h"
#include "syscall.h"

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
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER
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

enum struct virtio_gpu_formats : uint32_t {
    VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM = 1,
    VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM = 2,
    VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM = 3,
    VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM = 4,

    VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM = 67,
    VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM = 68,

    VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM = 121,
    VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM = 134
};

struct virtio_gpu_resource_create_2d : virtio_gpu_ctrl_hdr {
    uint32_t resource_id;
    virtio_gpu_formats format;
    uint32_t width;
    uint32_t height;
};

struct virtio_gpu_mem_entry {
    uint64_t addr;
    uint32_t length;
    uint32_t padding;
};

struct virtio_gpu_resource_attach_backing : virtio_gpu_ctrl_hdr {
    uint32_t resource_id;
    uint32_t nr_entries;
    virtio_gpu_mem_entry entries[];
};

struct virtio_gpu_set_scanout : virtio_gpu_ctrl_hdr {
    struct virtio_gpu_rect r;
    uint32_t scanout_id;
    uint32_t resource_id;
};

struct virtio_gpu_transfer_to_host_2d : virtio_gpu_ctrl_hdr {
    struct virtio_gpu_rect r;
    uint64_t offset;
    uint32_t resource_id;
    uint32_t padding;
};

struct virtio_gpu_resource_flush : virtio_gpu_ctrl_hdr {
    struct virtio_gpu_rect r;
    uint32_t resource_id;
    uint32_t padding;
};

enum struct virtio_input_config_select : uint8_t {
    VIRTIO_INPUT_CFG_UNSET = 0x00,
    VIRTIO_INPUT_CFG_ID_NAME = 0x01,
    VIRTIO_INPUT_CFG_ID_SERIAL = 0x02,
    VIRTIO_INPUT_CFG_ID_DEVIDS = 0x03,
    VIRTIO_INPUT_CFG_PROP_BITS = 0x10,
    VIRTIO_INPUT_CFG_EV_BITS = 0x11,
    VIRTIO_INPUT_CFG_ABS_INFO = 0x12
};

struct virtio_input_absinfo {
    uint32_t min;
    uint32_t max;
    uint32_t fuzz;
    uint32_t flat;
    uint32_t res;
};

struct virtio_input_devids {
    uint16_t bustype;
    uint16_t vendor;
    uint16_t product;
    uint16_t version;
};

struct virtio_input_config {
    virtio_input_config_select select;
    uint8_t subsel;
    uint8_t size;
    uint8_t reserved[5];
    union {
        char string[128];
        uint8_t bitmap[128];
        virtio_input_absinfo abs;
        virtio_input_devids ids;
    } u;
};

struct virtio_input_event {
    uint16_t type;
    uint16_t code;
    uint32_t value;
};

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

static bool init_virtio_device(
    size_t pci_device_location,
    size_t *result_configuration_address,
    volatile virtio_pci_common_cfg **result_common_configuration
) {
    auto configuration_address = syscall(SyscallType::MapPCIEConfiguration, pci_device_location, 0);
    if(configuration_address == 0) {
        return false;
    }

    auto configuration_header = (volatile PCIHeader*)configuration_address;

    auto common_config_capability = find_capability(configuration_address, 1);
    if(common_config_capability == nullptr) {
        return false;
    }

    auto common_bar_address = syscall(SyscallType::MapPCIEBar, common_config_capability->bar | pci_device_location << bar_index_bits, 0);
    if(common_bar_address == 0) {
        return false;
    }

    auto common_configuration = (volatile virtio_pci_common_cfg*)(common_bar_address + common_config_capability->offset);

    common_configuration->device_status = 0; // Reset device
    common_configuration->device_status |= 1; // Set ACKNOWLEDGE flag
    common_configuration->device_status |= 1 << 1; // Set DRIVER flag
    
    common_configuration->driver_feature_select = 0;
    common_configuration->driver_feature = 0;

    common_configuration->device_feature_select = 1;
    common_configuration->device_feature = 1; // Set VIRTIO_F_VERSION_1 flag

    common_configuration->device_status |= 1 << 3; // Set FEATURES_OK flag

    if((common_configuration->device_status & ~(1 << 3)) == 0) { // Check for FEATURES_OK flag
        return false;
    }

    *result_configuration_address = configuration_address;
    *result_common_configuration = common_configuration;
    return true;
}