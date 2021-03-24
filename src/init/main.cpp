#include <stdint.h>
#include <stddef.h>
#include "printf.h"
#include "syscall.h"
#include "pcie.h"
#include "secondary.h"
#include "virtio.h"

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

void _putchar(char character) {
    syscall(SyscallType::DebugPrint, character, 0);
}

const size_t queue_descriptor_count = 2;

const size_t buffer_size = 1024;

static volatile virtio_gpu_ctrl_hdr *send_command(
    size_t command_size,
    size_t buffers_address,
    volatile virtq_desc* queue_descriptors,
    volatile virtq_avail* available_ring,
    volatile virtq_used* used_ring,
    volatile uint16_t* notify
) {
    available_ring->ring[available_ring->idx % queue_descriptor_count] = 0;

    auto previous_used_index = used_ring->idx;

    available_ring->idx += 1;

    *notify = 0;

    while(used_ring->idx == previous_used_index);

    return (volatile virtio_gpu_ctrl_hdr*)(buffers_address + buffer_size);
}

extern uint8_t secondary_executable[];
extern uint8_t secondary_executable_end[];

extern "C" [[noreturn]] void entry(size_t process_id, void *data, size_t data_size) {
    size_t virtio_gpu_location;
    {
        FindPCIEDeviceParameters parameters {};
        parameters.require_vendor_id = true;
        parameters.vendor_id = 0x1AF4;
        parameters.require_device_id = true;
        parameters.device_id = 0x1050;

        if(syscall(
            SyscallType::FindPCIEDevice,
            (size_t)&parameters,
            0,
            &virtio_gpu_location
        ) != (size_t)FindPCIEDeviceResult::Success) {
            printf("Error: virtio-gpu device not present\n");

            exit();
        }
    }

    size_t configuration_address;
    volatile virtio_pci_common_cfg* common_configuration;
    if(!init_virtio_device(virtio_gpu_location, &configuration_address, &common_configuration)) {
        printf("Error: Unable to initialize virtio-gpu device\n");

        exit();
    }

    common_configuration->queue_select = 0; // Select controlq queue

    size_t queue_descriptors_physical_address;
    auto queue_descriptors = (volatile virtq_desc*)syscall(
        SyscallType::MapFreeConsecutiveMemory,
        sizeof(virtq_desc) * queue_descriptor_count,
        0,
        &queue_descriptors_physical_address
    );
    if(queue_descriptors == nullptr) {
        printf("Error: Unable to allocate memory for queue descriptor\n");

        exit();
    }

    common_configuration->queue_size = queue_descriptor_count; // Set controlq queue size

    queue_descriptors[0].flags = 0b1; // Set device readable (driver buffer), set has next
    queue_descriptors[0].next = 1; // Link to second descriptor

    queue_descriptors[1].flags = 0b10; // Set device writable (device buffer)

    size_t buffers_physical_address;
    auto buffers_address = syscall(SyscallType::MapFreeConsecutiveMemory, buffer_size * queue_descriptor_count, 0, &buffers_physical_address);
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
        0,
        &available_ring_physical_address
    );
    if(available_ring == nullptr) {
        printf("Error: Unable to allocate memory for available ring\n");

        exit();
    }

    size_t used_ring_physical_address;
    auto used_ring = (volatile virtq_used*)syscall(
        SyscallType::MapFreeConsecutiveMemory,
        sizeof(virtq_used) + sizeof(virtq_used_elem) * queue_descriptor_count,
        0,
        &used_ring_physical_address
    );
    if(used_ring == nullptr) {
        printf("Error: Unable to allocate memory for used ring\n");

        exit();
    }

    common_configuration->queue_desc = queue_descriptors_physical_address;
    common_configuration->queue_driver = available_ring_physical_address;
    common_configuration->queue_device = used_ring_physical_address;

    common_configuration->queue_enable = 1;

    auto notify_capability = (volatile virtio_pci_notify_cap*)find_capability(configuration_address, 2);
    if(notify_capability == nullptr) {
        printf("Error: virtio-gpu notify capability not found\n");

        exit();
    }

    auto notify_bar_address = syscall(SyscallType::MapPCIEBar, notify_capability->bar | virtio_gpu_location << bar_index_bits, 0);
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

    auto get_display_info_command = (volatile virtio_gpu_ctrl_hdr*)buffers_address;
    get_display_info_command->type = virtio_gpu_ctrl_type::VIRTIO_GPU_CMD_GET_DISPLAY_INFO;
    get_display_info_command->flags = 0;

    auto get_display_info_response = (volatile virtio_gpu_resp_display_info*)send_command(
        sizeof(virtio_gpu_ctrl_hdr),
        buffers_address,
        queue_descriptors,
        available_ring,
        used_ring,
        notify
    );

    if(get_display_info_response->type != virtio_gpu_ctrl_type::VIRTIO_GPU_RESP_OK_DISPLAY_INFO) {
        printf("Error: Invalid response from VIRTIO_GPU_CMD_GET_DISPLAY_INFO command\n");

        exit();
    }

    auto display_width = (size_t)get_display_info_response->pmodes[0].r.width;
    auto display_height = (size_t)get_display_info_response->pmodes[0].r.height;

    auto create_resource_command = (volatile virtio_gpu_resource_create_2d*)buffers_address;
    create_resource_command->type = virtio_gpu_ctrl_type::VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    create_resource_command->flags = 0;
    create_resource_command->resource_id = 1;
    create_resource_command->format = virtio_gpu_formats::VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM;
    create_resource_command->width = display_width;
    create_resource_command->height = display_height;

    auto create_resource_response = (volatile virtio_gpu_ctrl_hdr*)send_command(
        sizeof(virtio_gpu_resource_create_2d),
        buffers_address,
        queue_descriptors,
        available_ring,
        used_ring,
        notify
    );

    if(create_resource_response->type != virtio_gpu_ctrl_type::VIRTIO_GPU_RESP_OK_NODATA) {
        printf("Error: Invalid response from VIRTIO_GPU_CMD_RESOURCE_CREATE_2D command\n");

        exit();
    }

    auto display_framebuffer_size = display_height * display_width * 4;

    size_t framebuffer_physical_address;
    auto display_framebuffer_address = syscall(SyscallType::MapFreeConsecutiveMemory, display_framebuffer_size, 0, &framebuffer_physical_address);
    if(display_framebuffer_address == 0) {
        printf("Error: Unable to allocate memory for display framebuffer\n");

        exit();
    }

    auto attach_backing_command = (volatile virtio_gpu_resource_attach_backing*)buffers_address;
    attach_backing_command->type = virtio_gpu_ctrl_type::VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    attach_backing_command->flags = 0;
    attach_backing_command->resource_id = 1;
    attach_backing_command->nr_entries = 1;
    attach_backing_command->entries[0].addr = framebuffer_physical_address;
    attach_backing_command->entries[0].length = display_framebuffer_size;

    auto attach_backing_response = (volatile virtio_gpu_ctrl_hdr*)send_command(
        sizeof(virtio_gpu_resource_attach_backing) + sizeof(virtio_gpu_mem_entry),
        buffers_address,
        queue_descriptors,
        available_ring,
        used_ring,
        notify
    );

    if(attach_backing_response->type != virtio_gpu_ctrl_type::VIRTIO_GPU_RESP_OK_NODATA) {
        printf("Error: Invalid response from VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING command\n");

        exit();
    }

    auto set_scanout_command = (volatile virtio_gpu_set_scanout*)buffers_address;
    set_scanout_command->type = virtio_gpu_ctrl_type::VIRTIO_GPU_CMD_SET_SCANOUT;
    set_scanout_command->flags = 0;
    set_scanout_command->r.x = 0;
    set_scanout_command->r.y = 0;
    set_scanout_command->r.width = display_width;
    set_scanout_command->r.height = display_height;
    set_scanout_command->scanout_id = 0;
    set_scanout_command->resource_id = 1;

    auto set_scanout_response = (volatile virtio_gpu_ctrl_hdr*)send_command(
        sizeof(virtio_gpu_set_scanout),
        buffers_address,
        queue_descriptors,
        available_ring,
        used_ring,
        notify
    );

    if(set_scanout_response->type != virtio_gpu_ctrl_type::VIRTIO_GPU_RESP_OK_NODATA) {
        printf("Error: Invalid response from VIRTIO_GPU_CMD_SET_SCANOUT command\n");

        exit();
    }

    auto swap_indicator_address = syscall(SyscallType::CreateSharedMemory, 1, 0);
    if(swap_indicator_address == 0) {
        printf("Error: Unable to allocate shared memory for swap indicator\n");

        exit();
    }

    auto swap_indicator = (volatile bool*)swap_indicator_address;

    const size_t secondary_framebuffer_width = 300;
    const size_t secondary_framebuffer_height = 300;
    const size_t secondary_framebuffer_size = secondary_framebuffer_width * secondary_framebuffer_height * 4;

    auto secondary_framebuffers_address = syscall(SyscallType::CreateSharedMemory, secondary_framebuffer_size * 2, 0);
    if(secondary_framebuffers_address == 0) {
        printf("Error: Unable to allocate shared memory for secondary process framebuffers\n");

        exit();
    }

    auto secondary_framebuffer_x = 100;
    auto secondary_framebuffer_y = 200;

    {
        SecondaryProcessParameters process_parameters {
            process_id,
            secondary_framebuffers_address,
            secondary_framebuffer_width,
            secondary_framebuffer_height,
            swap_indicator_address
        };

        auto secondary_executable_size = (size_t)&secondary_executable_end - (size_t)&secondary_executable;

        CreateProcessParameters parameters {
            &secondary_executable,
            secondary_executable_size,
            &process_parameters,
            sizeof(SecondaryProcessParameters)
        };

        switch((CreateProcessResult)syscall(SyscallType::CreateProcess, (size_t)&parameters, 0)) {
            case CreateProcessResult::Success: break;

            case CreateProcessResult::OutOfMemory: {
                printf("Error: Unable to create secondary process: Out of memory\n");

                exit();
            } break;

            case CreateProcessResult::InvalidMemoryRange: {
                printf("Error: Unable to create secondary process: Invalid memory range for ELF binary or data\n");

                exit();
            } break;

            case CreateProcessResult::InvalidELF: {
                printf("Error: Unable to create secondary process: Invalid ELF binary\n");

                exit();
            } break;
        }
    }

    while(true) {
        memset((void*)display_framebuffer_address, 0, display_framebuffer_size);

        for(size_t y = 0; y < secondary_framebuffer_height; y += 1) {
            size_t current_secondary_framebuffer_address;
            if(*swap_indicator) {
                current_secondary_framebuffer_address = secondary_framebuffers_address + secondary_framebuffer_size;
            } else {
                current_secondary_framebuffer_address = secondary_framebuffers_address;
            }

            memcpy(
                (void*)(display_framebuffer_address + ((y + secondary_framebuffer_y) * display_width + secondary_framebuffer_x) * 4),
                (void*)(current_secondary_framebuffer_address + y * secondary_framebuffer_width * 4),
                secondary_framebuffer_width * 4
            );
        }

        auto transfer_command = (volatile virtio_gpu_transfer_to_host_2d*)buffers_address;
        transfer_command->type = virtio_gpu_ctrl_type::VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
        transfer_command->flags = 0;
        transfer_command->r.x = 0;
        transfer_command->r.y = 0;
        transfer_command->r.width = display_width;
        transfer_command->r.height = display_height;
        transfer_command->offset = 0;
        transfer_command->resource_id = 1;

        auto transfer_response = (volatile virtio_gpu_ctrl_hdr*)send_command(
            sizeof(virtio_gpu_transfer_to_host_2d),
            buffers_address,
            queue_descriptors,
            available_ring,
            used_ring,
            notify
        );

        if(transfer_response->type != virtio_gpu_ctrl_type::VIRTIO_GPU_RESP_OK_NODATA) {
            printf("Error: Invalid response from VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D command\n");

            exit();
        }

        auto flush_command = (volatile virtio_gpu_resource_flush*)buffers_address;
        flush_command->type = virtio_gpu_ctrl_type::VIRTIO_GPU_CMD_RESOURCE_FLUSH;
        flush_command->flags = 0;
        flush_command->r.x = 0;
        flush_command->r.y = 0;
        flush_command->r.width = display_width;
        flush_command->r.height = display_height;
        flush_command->resource_id = 1;

        auto flush_response = (volatile virtio_gpu_ctrl_hdr*)send_command(
            sizeof(virtio_gpu_resource_flush),
            buffers_address,
            queue_descriptors,
            available_ring,
            used_ring,
            notify
        );

        if(flush_response->type != virtio_gpu_ctrl_type::VIRTIO_GPU_RESP_OK_NODATA) {
            printf("Error: Invalid response from VIRTIO_GPU_CMD_RESOURCE_FLUSH command\n");

            exit();
        }
    }

    exit();
}

asm(
    ".section .rodata\n"
    "secondary_executable:\n"
    ".incbin \"build/init_secondary.elf\"\n"
    "secondary_executable_end:"
);