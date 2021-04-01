#include <stdint.h>
#include <stddef.h>
#include "printf.h"
#include "syscall.h"
#include "pcie.h"
#include "secondary.h"
#include "virtio.h"
#include "bucket_array.h"
#include "bucket_array_user.h"
#include "compositor.h"

#define bits_to_mask(bits) ((1 << (bits)) - 1)

#define min(a, b) ((a) > (b) ? (b) : (a))
#define max(a, b) ((a) < (b) ? (b) : (a))

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

size_t cursor_bitmap_size = 16;
extern uint32_t cursor_bitmap[];

struct ClientProcess {
    size_t process_id;

    volatile CompositorMailbox* mailbox;
    volatile CompositorRing* ring;
};

using ClientProcesses = BucketArray<ClientProcess, 4>;

struct Window {
    ClientProcess *client_process;

    size_t id;

    intptr_t x;
    intptr_t y;

    intptr_t width;
    intptr_t height;

    intptr_t framebuffer_width;
    intptr_t framebuffer_height;

    size_t z_index;

    volatile uint32_t *framebuffers;
    volatile bool *swap_indicator;
};

using Windows = BucketArray<Window, 4>;

static void send_size_changed_event(const Window *window) {
    auto ring = window->client_process->ring;

    if(ring->read_head != ring->write_head) {
        auto entry = &ring->entries[ring->write_head];

        entry->window_id = window->id;

        entry->type = CompositorEventType::SizeChanged;

        entry->size_changed.width = window->width;
        entry->size_changed.height = window->height;

        ring->write_head = (ring->write_head + 1) % compositor_ring_length;
    }
}

static void send_focus_lost_event(const Window *window) {
    auto ring = window->client_process->ring;

    if(ring->read_head != ring->write_head) {
        auto entry = &ring->entries[ring->write_head];

        entry->window_id = window->id;

        entry->type = CompositorEventType::FocusLost;

        ring->write_head = (ring->write_head + 1) % compositor_ring_length;
    }
}

extern "C" [[noreturn]] void entry(size_t process_id, void *data, size_t data_size) {
    struct VirtIOInputDevice {
        volatile virtq_avail *available_ring;

        volatile virtq_used *used_ring;

        size_t buffers_address;

        uint16_t previous_used_index;
    };

    using VirtIOInputDevices = BucketArray<VirtIOInputDevice, 4>;

    VirtIOInputDevices virtio_input_devices {};

    const size_t virtio_input_queue_descriptor_count = 32;
    const size_t virtio_input_buffer_size = 16;

    size_t virtio_input_index = 0;
    while(true) {
        size_t pcie_location;
        FindPCIEDeviceParameters parameters {};
        parameters.index = virtio_input_index;
        parameters.require_vendor_id = true;
        parameters.vendor_id = 0x1AF4;
        parameters.require_device_id = true;
        parameters.device_id = 0x1052;

        if(syscall(
            SyscallType::FindPCIEDevice,
            (size_t)&parameters,
            0,
            &pcie_location
        ) != (size_t)FindPCIEDeviceResult::Success) {
            break;
        }

        auto device = allocate_from_bucket_array(&virtio_input_devices);
        if(device == nullptr) {
            printf("Error: Out of memory\n");

            exit();
        }

        size_t configuration_address;
        volatile virtio_pci_common_cfg* common_configuration;
        if(!init_virtio_device(pcie_location, &configuration_address, &common_configuration)) {
            printf("Error: Unable to initialize virtio-input device\n");

            exit();
        }

        common_configuration->queue_select = 0; // Select eventq queue

        size_t queue_descriptors_physical_address;
        auto queue_descriptors = (volatile virtq_desc*)syscall(
            SyscallType::MapFreeConsecutiveMemory,
            sizeof(virtq_desc) * virtio_input_queue_descriptor_count,
            0,
            &queue_descriptors_physical_address
        );
        if(queue_descriptors == nullptr) {
            printf("Error: Unable to allocate memory for queue descriptor\n");

            exit();
        }

        common_configuration->queue_size = virtio_input_queue_descriptor_count; // Set eventq queue size

        size_t buffers_physical_address;
        auto buffers_address = syscall(SyscallType::MapFreeConsecutiveMemory, virtio_input_buffer_size * virtio_input_queue_descriptor_count, 0, &buffers_physical_address);
        if(buffers_address == 0) {
            printf("Error: Unable to allocate memory for queue buffers\n");

            exit();
        }

        for(size_t i = 0; i < virtio_input_queue_descriptor_count; i += 1) {
            queue_descriptors[i].flags |= 1 << 1; // Set VIRTQ_DESC_F_WRITE flag

            queue_descriptors[i].addr = buffers_physical_address + virtio_input_buffer_size * i;
            queue_descriptors[i].len = virtio_input_buffer_size;
        }

        size_t available_ring_physical_address;
        auto available_ring = (volatile virtq_avail*)syscall(
            SyscallType::MapFreeConsecutiveMemory,
            sizeof(virtq_avail) + sizeof(uint16_t) * virtio_input_queue_descriptor_count,
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
            sizeof(virtq_used) + sizeof(virtq_used_elem) * virtio_input_queue_descriptor_count,
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

        common_configuration->device_status |= 1 << 2; // Set DRIVER_OK flag

        auto previous_used_index = used_ring->idx;

        for(size_t i = 0; i < virtio_input_queue_descriptor_count; i += 1) {
            available_ring->ring[available_ring->idx % virtio_input_queue_descriptor_count] = i;

            available_ring->idx += 1;
        }

        device->available_ring = available_ring;
        device->used_ring = used_ring;
        device->buffers_address = buffers_address;
        device->previous_used_index = previous_used_index;

        virtio_input_index += 1;
    }

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

    ClientProcesses client_processes {};

    Windows windows {};

    size_t next_window_id = 0;

    auto connection_mailbox_address = syscall(SyscallType::CreateSharedMemory, sizeof(CompositorConnectionMailbox), 0);
    if(connection_mailbox_address == 0) {
        printf("Error: Unable to allocate shared memory for compositor connection mailbox\n");

        exit();
    }

    auto connection_mailbox = (volatile CompositorConnectionMailbox*)connection_mailbox_address;

    SecondaryProcessParameters secondary_process_parameters {
        process_id,
        connection_mailbox_address
    };

    auto secondary_executable_size = (size_t)&secondary_executable_end - (size_t)&secondary_executable;

    for(size_t i = 0; i < 2; i += 1) {
        CreateProcessParameters parameters {
            &secondary_executable,
            secondary_executable_size,
            &secondary_process_parameters,
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

    const intptr_t window_title_height = 32;

    enum struct WindowSide {
        Top,
        Bottom,
        Left,
        Right
    };

    Window *focused_window = nullptr;
    bool dragging_focused_window;
    bool resizing_focused_window;
    WindowSide resizing_side;

    intptr_t cursor_x = (intptr_t)display_width / 2;
    intptr_t cursor_y = (intptr_t)display_height / 2;
    auto previous_cursor_x = cursor_x;
    auto previous_cursor_y = cursor_y;

    while(true) {
        if(connection_mailbox->locked && connection_mailbox->connection_requested) {
            do { // Purely so break can be used for early-out
                if(!(bool)syscall(SyscallType::DoesProcessExist, connection_mailbox->process_id, 0)) {
                    connection_mailbox->result = CompositorConnectionResult::InvalidProcessID;

                    break;
                }

                auto mailbox_address = syscall(SyscallType::CreateSharedMemory, sizeof(CompositorMailbox), 0);
                if(mailbox_address == 0) {
                    connection_mailbox->result = CompositorConnectionResult::OutOfMemory;

                    break;
                }

                auto mailbox = (volatile CompositorMailbox*)mailbox_address;

                auto ring_address = syscall(SyscallType::CreateSharedMemory, sizeof(CompositorRing), 0);
                if(ring_address == 0) {
                    syscall(SyscallType::UnmapMemory, mailbox_address, 0);

                    connection_mailbox->result = CompositorConnectionResult::OutOfMemory;

                    break;
                }

                auto ring = (volatile CompositorRing*)ring_address;

                auto client_process = allocate_from_bucket_array(&client_processes);
                if(client_process == nullptr) {
                    syscall(SyscallType::UnmapMemory, mailbox_address, 0);
                    syscall(SyscallType::UnmapMemory, ring_address, 0);

                    connection_mailbox->result = CompositorConnectionResult::OutOfMemory;

                    break;
                }

                client_process->process_id = connection_mailbox->process_id;
                client_process->mailbox = mailbox;
                client_process->ring = ring;

                connection_mailbox->mailbox_shared_memory = mailbox_address;
                connection_mailbox->ring_shared_memory = ring_address;

                connection_mailbox->result = CompositorConnectionResult::Success;
            } while(false);

            connection_mailbox->connection_requested = false;
        }

        for(auto client_process_iterator = begin(client_processes); client_process_iterator != end(client_processes); ++client_process_iterator) {
            auto client_process = *client_process_iterator;

            if(!(bool)syscall(SyscallType::DoesProcessExist, client_process->process_id, 0)) {
                for(auto window_iterator = begin(windows); window_iterator != end(windows); ++window_iterator) {
                    auto window = *window_iterator;

                    if(window->client_process == client_process) {
                        if(window == focused_window) {
                            focused_window = nullptr;
                        }

                        remove_item_from_bucket_array(window_iterator);
                    }
                }

                syscall(SyscallType::UnmapMemory, (size_t)client_process->mailbox, 0);
                syscall(SyscallType::UnmapMemory, (size_t)client_process->ring, 0);

                remove_item_from_bucket_array(client_process_iterator);

                continue;
            }

            auto mailbox = client_process->mailbox;
            auto ring = client_process->ring;

            if(mailbox->command_present){
                switch(mailbox->command_type) {
                    case CompositorCommandType::CreateWindow: {
                        auto command = &mailbox->create_window;

                        if(command->width <= 0 || command->height <= 0) {
                            command->result = CreateWindowResult::InvalidSize;

                            mailbox->command_present = false;

                            break;
                        }

                        auto swap_indicator_address = syscall(SyscallType::CreateSharedMemory, sizeof(bool), 0);
                        if(swap_indicator_address == 0) {
                            command->result = CreateWindowResult::OutOfMemory;

                            mailbox->command_present = false;

                            break;
                        }

                        auto framebuffers_address = syscall(SyscallType::CreateSharedMemory, command->width * command->height * 4 * 2, 0);
                        if(framebuffers_address == 0) {
                            syscall(SyscallType::UnmapMemory, swap_indicator_address, 0);

                            command->result = CreateWindowResult::OutOfMemory;

                            mailbox->command_present = false;

                            break;
                        }

                        auto window = allocate_from_bucket_array(&windows);
                        if(window == nullptr) {
                            syscall(SyscallType::UnmapMemory, swap_indicator_address, 0);
                            syscall(SyscallType::UnmapMemory, framebuffers_address, 0);

                            command->result = CreateWindowResult::OutOfMemory;

                            mailbox->command_present = false;

                            break;
                        }

                        auto any_windows = false;
                        size_t highest_window_z_index;
                        for(auto window : windows) {
                            if(!any_windows || window->z_index > highest_window_z_index) {
                                highest_window_z_index = window->z_index;
                                any_windows = true;
                            }
                        }

                        window->client_process = client_process;

                        window->id = next_window_id;
                        next_window_id += 1;

                        window->x = command->x;
                        window->y = command->y;
                        window->width = command->width;
                        window->height = command->height;
                        window->framebuffer_width = command->width;
                        window->framebuffer_height = command->height;

                        if(any_windows) {
                            window->z_index = highest_window_z_index + 1;
                        }

                        window->framebuffers = (volatile uint32_t*)framebuffers_address;
                        window->swap_indicator = (volatile bool*)swap_indicator_address;

                        command->result = CreateWindowResult::Success;

                        command->id = window->id;
                        command->framebuffers_shared_memory = framebuffers_address;
                        command->swap_indicator_shared_memory = swap_indicator_address;

                        mailbox->command_present = false;

                        if(focused_window != nullptr) {
                            if(resizing_focused_window) {
                                send_size_changed_event(focused_window);

                                resizing_focused_window = false;
                            }

                            dragging_focused_window = false;

                            send_focus_lost_event(focused_window);
                        }

                        focused_window = window;

                        if(ring->read_head != ring->write_head) {
                            auto entry = &ring->entries[ring->write_head];

                            entry->window_id = focused_window->id;

                            entry->type = CompositorEventType::FocusGained;

                            entry->focus_gained.mouse_x = cursor_x - focused_window->x;
                            entry->focus_gained.mouse_y = cursor_y - focused_window->y - window_title_height;

                            ring->write_head = (ring->write_head + 1) % compositor_ring_length;
                        }
                    } break;

                    case CompositorCommandType::DestroyWindow: {
                        auto command = &mailbox->destroy_window;

                        for(auto window_iterator = begin(windows); window_iterator != end(windows); ++window_iterator) {
                            auto window = *window_iterator;

                            if(window->id == command->id) {
                                if(window == focused_window) {
                                    focused_window = nullptr;
                                }

                                syscall(SyscallType::UnmapMemory, (size_t)window->framebuffers, 0);
                                syscall(SyscallType::UnmapMemory, (size_t)window->swap_indicator, 0);

                                remove_item_from_bucket_array(window_iterator);

                                break;
                            }
                        }

                        mailbox->command_present = false;
                    } break;

                    case CompositorCommandType::ResizeFramebuffers: {
                        auto command = &mailbox->resize_framebuffers;

                        if(command->width <= 0 || command->height <= 0) {
                            command->result = ResizeFramebuffersResult::InvalidSize;

                            mailbox->command_present = false;

                            break;
                        }

                        command->result = ResizeFramebuffersResult::InvalidWindowID;

                        for(auto window_iterator = begin(windows); window_iterator != end(windows); ++window_iterator) {
                            auto window = *window_iterator;

                            if(window->id == command->id) {
                                auto framebuffers_address = syscall(SyscallType::CreateSharedMemory, command->width * command->height * 4 * 2, 0);
                                if(framebuffers_address == 0) {
                                    command->result = ResizeFramebuffersResult::OutOfMemory;

                                    mailbox->command_present = false;

                                    break;
                                }

                                syscall(SyscallType::UnmapMemory, (size_t)window->framebuffers, 0);

                                window->framebuffer_width = command->width;
                                window->framebuffer_height = command->height;
                                window->framebuffers = (volatile uint32_t*)framebuffers_address;

                                command->framebuffers_shared_memory = framebuffers_address;

                                command->result = ResizeFramebuffersResult::Success;

                                break;
                            }
                        }

                        mailbox->command_present = false;
                    } break;

                    default: {
                        printf("Error: Unknown compositor command type %u from client process %zu\n", mailbox->command_type, client_process->process_id);
                    } break;
                }
            }
        }

        for(auto device : virtio_input_devices) {
            auto used_index = device->used_ring->idx;

            if(used_index != device->previous_used_index) {
                for(uint16_t index = device->previous_used_index; index != used_index; index += 1) {
                    auto descriptor_index = (size_t)device->used_ring->ring[index % virtio_input_queue_descriptor_count].id;

                    auto event = (volatile virtio_input_event*)(device->buffers_address + virtio_input_buffer_size * descriptor_index);

                    switch(event->type) {
                        case 1: { // EV_KEY
                            auto key_state = (bool)event->value;
                            auto is_mouse_button = (event->code >= 0x110 && event->code <= 0x117) || (event->code >= 0x140 && event->code <= 0x14F);

                            if(key_state && is_mouse_button) {
                                auto any_windows = false;
                                size_t highest_window_z_index;
                                Window *highest_intersecting_window = nullptr;
                                for(auto window : windows) {
                                    if(!any_windows || window->z_index > highest_window_z_index) {
                                        highest_window_z_index = window->z_index;
                                        any_windows = true;
                                    }

                                    if(
                                        cursor_x >= window->x && cursor_y >= window->y &&
                                        cursor_x < window->x + window->width && cursor_y < window->y + window->height + window_title_height
                                    ) {
                                        if(highest_intersecting_window == nullptr || window->z_index > highest_intersecting_window->z_index) {
                                            highest_intersecting_window = window;
                                        }
                                    }
                                }

                                if(!any_windows || highest_intersecting_window != focused_window) {
                                    if(focused_window != nullptr) {
                                        if(resizing_focused_window) {
                                            send_size_changed_event(focused_window);

                                            resizing_focused_window = false;
                                        }

                                        dragging_focused_window = false;

                                        send_focus_lost_event(focused_window);
                                    }

                                    focused_window = highest_intersecting_window;
                                }

                                if(
                                    any_windows &&
                                    highest_intersecting_window != nullptr &&
                                    highest_intersecting_window->z_index != highest_window_z_index
                                ) {
                                    highest_intersecting_window->z_index = highest_window_z_index + 1;
                                }
                            }

                            intptr_t resize_region_size = 8;

                            if(focused_window != nullptr) {
                                auto ring = focused_window->client_process->ring;

                                auto forward_event = true;
                                if(is_mouse_button) {
                                    if(cursor_x < focused_window->x + resize_region_size) {
                                        if(event->code == 0x110) { // BTN_LEFT
                                            if(key_state) {
                                                resizing_focused_window = key_state;
                                                resizing_side = WindowSide::Left;

                                                forward_event = false;
                                            } else {
                                                if(resizing_focused_window) {
                                                    send_size_changed_event(focused_window);

                                                    resizing_focused_window = false;
                                                }
                                            }
                                        }
                                    } else if(cursor_y < focused_window->y + resize_region_size) {
                                        if(event->code == 0x110) { // BTN_LEFT
                                            if(key_state) {
                                                resizing_focused_window = key_state;
                                                resizing_side = WindowSide::Top;

                                                forward_event = false;
                                            } else {
                                                if(resizing_focused_window) {
                                                    send_size_changed_event(focused_window);

                                                    resizing_focused_window = false;
                                                }
                                            }
                                        }
                                    } else if(cursor_x >= focused_window->x + focused_window->width - resize_region_size) {
                                        if(event->code == 0x110) { // BTN_LEFT
                                            if(key_state) {
                                                resizing_focused_window = key_state;
                                                resizing_side = WindowSide::Right;

                                                forward_event = false;
                                            } else {
                                                if(resizing_focused_window) {
                                                    send_size_changed_event(focused_window);

                                                    resizing_focused_window = false;
                                                }
                                            }
                                        }
                                    } else if(cursor_y >= focused_window->y + focused_window->height + window_title_height - resize_region_size) {
                                        if(event->code == 0x110) { // BTN_LEFT
                                            if(key_state) {
                                                resizing_focused_window = key_state;
                                                resizing_side = WindowSide::Bottom;

                                                forward_event = false;
                                            } else {
                                                if(resizing_focused_window) {
                                                    send_size_changed_event(focused_window);

                                                    resizing_focused_window = false;
                                                }
                                            }
                                        }
                                    } else if(cursor_y < focused_window->y + window_title_height) {
                                        if(event->code == 0x110) { // BTN_LEFT
                                            if(cursor_x > focused_window->x + focused_window->width - window_title_height) {
                                                if(key_state && ring->read_head != ring->write_head) {
                                                    auto entry = &ring->entries[ring->write_head];

                                                    entry->window_id = focused_window->id;

                                                    entry->type = CompositorEventType::CloseRequested;

                                                    ring->write_head = (ring->write_head + 1) % compositor_ring_length;
                                                }
                                            } else {           
                                                dragging_focused_window = key_state;
                                            }
                                        }

                                        forward_event = false;
                                    }
                                }

                                if(forward_event && ring->read_head != ring->write_head) {
                                    auto entry = &ring->entries[ring->write_head];

                                    entry->window_id = focused_window->id;

                                    if(key_state) {
                                        entry->type = CompositorEventType::KeyDown;
                                        entry->key_down.scancode = event->code;
                                    } else {
                                        entry->type = CompositorEventType::KeyUp;
                                        entry->key_down.scancode = event->code;
                                    }

                                    ring->write_head = (ring->write_head + 1) % compositor_ring_length;
                                }
                            }
                        } break;

                        case 2: { // EV_REL
                            intptr_t dx = 0;
                            intptr_t dy = 0;

                            switch(event->code) {
                                case 0: { // REL_X
                                    dx = (int32_t)event->value;
                                } break;

                                case 1: { // REL_Y
                                    dy = (int32_t)event->value;
                                } break;
                            }

                            cursor_x += dx;
                            cursor_y += dy;

                            cursor_x = min(max(cursor_x, 0), (intptr_t)display_width);
                            cursor_y = min(max(cursor_y, 0), (intptr_t)display_height);

                            auto cursor_x_difference = cursor_x - previous_cursor_x;
                            auto cursor_y_difference = cursor_y - previous_cursor_y;

                            previous_cursor_x = cursor_x;
                            previous_cursor_y = cursor_y;

                            if(focused_window != nullptr) {
                                auto ring = focused_window->client_process->ring;

                                if(dragging_focused_window) {
                                    focused_window->x += cursor_x_difference;
                                    focused_window->y += cursor_y_difference;
                                } else if(resizing_focused_window) {
                                    switch(resizing_side) {
                                        case WindowSide::Top: {
                                            focused_window->y += cursor_y_difference;
                                            focused_window->height -= cursor_y_difference;
                                        } break;

                                        case WindowSide::Bottom: {
                                            focused_window->height += cursor_y_difference;
                                        } break;

                                        case WindowSide::Left: {
                                            focused_window->x += cursor_x_difference;
                                            focused_window->width -= cursor_x_difference;
                                        } break;

                                        case WindowSide::Right: {
                                            focused_window->width += cursor_x_difference;
                                        } break;
                                    }
                                } else if(ring->read_head != ring->write_head) {
                                    auto entry = &ring->entries[ring->write_head];

                                    entry->window_id = focused_window->id;

                                    entry->type = CompositorEventType::MouseMove;

                                    entry->mouse_move.x = cursor_x - focused_window->x;
                                    entry->mouse_move.dx = dx;
                                    entry->mouse_move.y = cursor_y - focused_window->y - window_title_height;
                                    entry->mouse_move.dy = dy;

                                    ring->write_head = (ring->write_head + 1) % compositor_ring_length;
                                }
                            }
                        } break;
                    }

                    device->available_ring->ring[device->available_ring->idx % virtio_input_queue_descriptor_count] = descriptor_index;

                    device->available_ring->idx += 1;
                }

                device->previous_used_index = device->used_ring->idx;
            }
        }

        memset((void*)display_framebuffer_address, 0, display_framebuffer_size);

        auto next_min_z_index = 0;
        while(true) {
            Window *next_window = nullptr;
            for(auto window : windows) {
                if(window->z_index >= next_min_z_index && (next_window == nullptr || window->z_index < next_window->z_index)) {
                    next_window = window;
                }
            }

            if(next_window == nullptr) {
                break;
            }

            next_min_z_index = next_window->z_index + 1;

            {
                auto top = next_window->y;
                auto bottom = top + window_title_height;

                auto left = next_window->x;
                auto right = left + next_window->width;

                auto visible_top = (size_t)min(max(top, 0), (intptr_t)display_height);
                auto visible_bottom = (size_t)max(min(bottom, (intptr_t)display_height), 0);

                auto visible_left = (size_t)min(max(left, 0), (intptr_t)display_width);
                auto visible_right = (size_t)max(min(right, (intptr_t)display_width), 0);

                auto visible_width = visible_right - visible_left;

                uint8_t shade;
                if(next_window == focused_window) {
                    shade = 0xFF;
                } else {
                    shade = 0xCC;
                }

                if(visible_width != 0) {
                    for(auto y = visible_top; y < visible_bottom; y += 1) {
                        memset(
                            (void*)(display_framebuffer_address + (y * display_width + visible_left) * 4),
                            shade,
                            visible_width * 4
                        );
                    }
                }
            }

            {
                auto top = next_window->y;
                auto bottom = top + window_title_height;

                auto left = next_window->x + next_window->width - window_title_height;
                auto right = left + window_title_height;

                auto visible_top = (size_t)min(max(top, 0), (intptr_t)display_height);
                auto visible_bottom = (size_t)max(min(bottom, (intptr_t)display_height), 0);

                auto visible_left = (size_t)min(max(left, 0), (intptr_t)display_width);
                auto visible_right = (size_t)max(min(right, (intptr_t)display_width), 0);

                uint32_t color;
                if(next_window == focused_window) {
                    color = 0xFF;
                } else {
                    color = 0xCC;
                }

                if(visible_right != visible_left) {
                    for(auto y = visible_top; y < visible_bottom; y += 1) {
                        for(auto x = visible_left; x < visible_right; x += 1) {
                            ((uint32_t*)display_framebuffer_address)[y * display_width + x] = color;
                        }
                    }
                }
            }

            auto framebuffer_size = next_window->framebuffer_width * next_window->framebuffer_height * 4;

            {
                auto top = next_window->y + window_title_height;
                auto bottom = top + min(next_window->height, next_window->framebuffer_height);

                auto left = next_window->x;
                auto right = left + min(next_window->width, next_window->framebuffer_width);

                auto visible_top = (size_t)min(max(top, 0), (intptr_t)display_height);
                auto visible_bottom = (size_t)max(min(bottom, (intptr_t)display_height), 0);

                auto visible_left = (size_t)min(max(left, 0), (intptr_t)display_width);
                auto visible_right = (size_t)max(min(right, (intptr_t)display_width), 0);

                auto visible_height = visible_bottom - visible_top;
                auto visible_width = visible_right - visible_left;

                auto visible_top_relative = (size_t)(visible_top - top);

                auto visible_left_relative = (size_t)(visible_left - left);

                if(visible_width != 0) {
                    for(size_t y = 0; y < visible_height; y += 1) {
                        // Aquire the current swapbuffer again for this line to prevent flickering (double buffering)
                        auto current_framebuffer_address = (size_t)next_window->framebuffers + (size_t)*next_window->swap_indicator * framebuffer_size;

                        memcpy(
                            (void*)(display_framebuffer_address + ((visible_top + y) * display_width + visible_left) * 4),
                            (void*)(current_framebuffer_address + ((visible_top_relative + y) * next_window->framebuffer_width + visible_left_relative) * 4),
                            visible_width * 4
                        );
                    }
                }
            }
        }

        for(size_t y = 0; y < cursor_bitmap_size; y += 1) {
            for(size_t x = 0; x < cursor_bitmap_size; x += 1) {
                auto absolute_x = x + cursor_x;
                auto absolute_y = y + cursor_y;

                if(absolute_x >= 0 && absolute_y >= 0 && absolute_x < (intptr_t)display_width && absolute_y < (intptr_t)display_height) {
                    auto pixel = cursor_bitmap[y * cursor_bitmap_size + x];

                    if(pixel >> 24 != 0) {
                        ((uint32_t*)display_framebuffer_address)[(size_t)absolute_y * display_width + (size_t)absolute_x] = pixel;
                    }
                }
            }
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

asm(
    ".section .rodata\n"
    "cursor_bitmap:\n"
    ".incbin \"src/init/cursor.raw\"\n"
);