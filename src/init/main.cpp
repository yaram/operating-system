#include <stdint.h>
#include <stddef.h>
#include "printf.h"
#include "syscall.h"
#include "pcie.h"
#include "secondary.h"
#include "virtio.h"
#include "bucket_array.h"
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

template <typename T, size_t N>
static T *allocate_from_bucket_array(
    BucketArray<T, N> *bucket_array,
    BucketArrayIterator<T, N> *result_iterator = nullptr
) {
    auto iterator = find_unoccupied_bucket_slot(bucket_array);

    if(iterator.current_bucket == nullptr) {
        auto new_bucket = (Bucket<T, N>*)syscall(SyscallType::MapFreeMemory, sizeof(Bucket<T, N>), 0);
        if(new_bucket == nullptr) {
            return nullptr;
        }

        {
            auto current_bucket = &bucket_array->first_bucket;
            while(current_bucket->next != nullptr) {
                current_bucket = current_bucket->next;
            }

            current_bucket->next = new_bucket;
        }

        iterator = {
            new_bucket,
            0
        };
    } else {
        memset(*iterator, 0, sizeof(T));
    }

    iterator.current_bucket->occupied[iterator.current_sub_index] = true;

    if(result_iterator != nullptr) {
        *result_iterator = iterator;
    }

    return *iterator;
}

extern uint8_t secondary_executable[];
extern uint8_t secondary_executable_end[];

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

    struct Window {
        size_t owner_process_id;

        size_t id;

        intptr_t x;
        intptr_t y;

        intptr_t width;
        intptr_t height;

        volatile uint32_t *framebuffers;
        volatile bool *swap_indicator;
    };

    using Windows = BucketArray<Window, 4>;

    Windows windows {};

    size_t next_window_id = 0;

    auto secondary_process_mailbox_address = syscall(SyscallType::CreateSharedMemory, sizeof(CompositorMailbox), 0);
    if(secondary_process_mailbox_address == 0) {
        printf("Error: Unable to allocate shared memory for compositor mailbox\n");

        exit();
    }

    auto secondary_process_mailbox = (volatile CompositorMailbox*)secondary_process_mailbox_address;

    auto secondary_process_ring_address = syscall(SyscallType::CreateSharedMemory, sizeof(CompositorRing), 0);
    if(secondary_process_ring_address == 0) {
        printf("Error: Unable to allocate shared memory for compositor ring\n");

        exit();
    }

    auto secondary_process_ring = (volatile CompositorRing*)secondary_process_ring_address;

    SecondaryProcessParameters secondary_process_parameters {
        process_id,
        secondary_process_mailbox_address,
        secondary_process_ring_address
    };

    auto secondary_executable_size = (size_t)&secondary_executable_end - (size_t)&secondary_executable;

    size_t secondary_process_id;
    {
        CreateProcessParameters parameters {
            &secondary_executable,
            secondary_executable_size,
            &secondary_process_parameters,
            sizeof(SecondaryProcessParameters)
        };

        switch((CreateProcessResult)syscall(SyscallType::CreateProcess, (size_t)&parameters, 0, &secondary_process_id)) {
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

    Window *focused_window = nullptr;

    intptr_t mouse_x = 0;
    intptr_t mouse_y = 0;

    while(true) {
        for(auto device : virtio_input_devices) {
            auto used_index = device->used_ring->idx;

            if(used_index != device->previous_used_index) {
                for(uint16_t index = device->previous_used_index; index != used_index; index += 1) {
                    auto descriptor_index = (size_t)device->used_ring->ring[index % virtio_input_queue_descriptor_count].id;

                    auto event = (volatile virtio_input_event*)(device->buffers_address + virtio_input_buffer_size * descriptor_index);

                    switch(event->type) {
                        case 1: { // EV_KEY
                            if(focused_window != nullptr) {
                                auto entry = &secondary_process_ring->entries[secondary_process_ring->write_head];

                                entry->window_id = focused_window->id;

                                if((bool)event->value) {
                                    entry->type = CompositorEventType::KeyDown;
                                    entry->key_down.scancode = event->code;

                                    if(event->code == 1) { // KEY_ESC
                                        if(focused_window != nullptr) {
                                            if(secondary_process_ring->read_head != secondary_process_ring->write_head) {
                                                auto entry = &secondary_process_ring->entries[secondary_process_ring->write_head];

                                                entry->window_id = focused_window->id;

                                                entry->type = CompositorEventType::FocusLost;

                                                secondary_process_ring->write_head = (secondary_process_ring->write_head + 1) % compositor_ring_length;
                                            }
                                        }

                                        focused_window = nullptr;
                                    }
                                } else {
                                    entry->type = CompositorEventType::KeyUp;
                                    entry->key_down.scancode = event->code;
                                }

                                secondary_process_ring->write_head = (secondary_process_ring->write_head + 1) % compositor_ring_length;
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

                            mouse_x += dx;
                            mouse_y += dy;

                            mouse_x = min(max(mouse_x, 0), (intptr_t)display_width);
                            mouse_y = min(max(mouse_y, 0), (intptr_t)display_height);

                            if(focused_window != nullptr && secondary_process_ring->read_head != secondary_process_ring->write_head) {
                                auto entry = &secondary_process_ring->entries[secondary_process_ring->write_head];

                                entry->window_id = focused_window->id;

                                entry->type = CompositorEventType::MouseMove;

                                entry->mouse_move.x = mouse_x - focused_window->x;
                                entry->mouse_move.dx = dx;
                                entry->mouse_move.y = mouse_y - focused_window->y;
                                entry->mouse_move.dy = dy;

                                secondary_process_ring->write_head = (secondary_process_ring->write_head + 1) % compositor_ring_length;
                            }
                        } break;
                    }

                    device->available_ring->ring[device->available_ring->idx % virtio_input_queue_descriptor_count] = descriptor_index;

                    device->available_ring->idx += 1;
                }

                device->previous_used_index = device->used_ring->idx;
            }
        }

        if(secondary_process_mailbox->command_present){
            switch(secondary_process_mailbox->command_type) {
                case CompositorCommandType::CreateWindow: {
                    auto command = &secondary_process_mailbox->create_window;

                    if(command->width <= 0 || command->height <= 0) {
                        command->result = CreateWindowResult::InvalidSize;

                        secondary_process_mailbox->command_present = false;

                        break;
                    }

                    auto swap_indicator_address = syscall(SyscallType::CreateSharedMemory, sizeof(bool), 0);
                    if(swap_indicator_address == 0) {
                        command->result = CreateWindowResult::OutOfMemory;

                        secondary_process_mailbox->command_present = false;

                        break;
                    }

                    auto framebuffers_address = syscall(SyscallType::CreateSharedMemory, command->width * command->height * 4 * 2, 0);
                    if(framebuffers_address == 0) {
                        command->result = CreateWindowResult::OutOfMemory;

                        secondary_process_mailbox->command_present = false;

                        break;
                    }

                    auto window = allocate_from_bucket_array(&windows);
                    if(window == nullptr) {
                        command->result = CreateWindowResult::OutOfMemory;

                        secondary_process_mailbox->command_present = false;

                        break;
                    }

                    window->owner_process_id = secondary_process_id;

                    window->id = next_window_id;
                    next_window_id += 1;

                    window->x = command->x;
                    window->y = command->y;
                    window->width = command->width;
                    window->height = command->height;

                    window->framebuffers = (volatile uint32_t*)framebuffers_address;
                    window->swap_indicator = (volatile bool*)swap_indicator_address;

                    command->result = CreateWindowResult::Success;

                    command->id = window->id;
                    command->framebuffers_shared_memory = framebuffers_address;
                    command->swap_indicator_shared_memory = swap_indicator_address;

                    secondary_process_mailbox->command_present = false;

                    if(focused_window != nullptr) {
                        if(secondary_process_ring->read_head != secondary_process_ring->write_head) {
                            auto entry = &secondary_process_ring->entries[secondary_process_ring->write_head];

                            entry->window_id = focused_window->id;

                            entry->type = CompositorEventType::FocusLost;

                            secondary_process_ring->write_head = (secondary_process_ring->write_head + 1) % compositor_ring_length;
                        }
                    }

                    focused_window = window;

                    if(secondary_process_ring->read_head != secondary_process_ring->write_head) {
                        auto entry = &secondary_process_ring->entries[secondary_process_ring->write_head];

                        entry->window_id = focused_window->id;

                        entry->type = CompositorEventType::FocusGained;

                        entry->focus_gained.mouse_x = mouse_x - focused_window->x;
                        entry->focus_gained.mouse_y = mouse_y - focused_window->y;

                        secondary_process_ring->write_head = (secondary_process_ring->write_head + 1) % compositor_ring_length;
                    }
                } break;

                case CompositorCommandType::DestroyWindow: {
                    auto command = &secondary_process_mailbox->destroy_window;

                    for(auto window_iterator = begin(windows); window_iterator != end(windows); ++window_iterator) {
                        auto window = *window_iterator;

                        if(window->id == command->id) {
                            remove_item_from_bucket_array(window_iterator);
                            break;
                        }
                    }
                } break;

                default: {
                    printf("Error: Unknown compositor command type %u\n", secondary_process_mailbox->command_type);
                } break;
            }
        }

        memset((void*)display_framebuffer_address, 0, display_framebuffer_size);

        for(auto window_iterator = begin(windows); window_iterator != end(windows); ++window_iterator) {
            auto window = *window_iterator;

            if(!(bool)syscall(SyscallType::DoesProcessExist, window->owner_process_id, 0)) {
                remove_item_from_bucket_array(window_iterator);

                continue;
            }

            auto framebuffer_size = window->width * window->height * sizeof(uint32_t);

            auto window_top = window->y;
            auto window_bottom = window->y + window->height;

            auto window_left = window->x;
            auto window_right = window->x + window->width;

            auto visible_top = (size_t)max(window_top, 0);
            auto visible_bottom = (size_t)min(window_bottom, (intptr_t)display_height);

            auto visible_left = (size_t)max(window_left, 0);
            auto visible_right = (size_t)min(window_right, (intptr_t)display_width);

            auto visible_height = visible_bottom - visible_top;
            auto visible_width = visible_right - visible_left;

            auto visible_top_relative = visible_top - window_top;

            auto visible_left_relative = visible_left - window_left;

            if(visible_width == 0) { // Window is not on the screen (offscreen on the x axis)
                continue;
            }

            for(size_t y = 0; y < visible_height; y += 1) {
                // Aquire the current swapbuffer again for this line to prevent flickering (double buffering)
                auto current_framebuffer_address = (size_t)window->framebuffers + (size_t)*window->swap_indicator * framebuffer_size;

                memcpy(
                    (void*)(display_framebuffer_address + ((visible_top + y) * display_width + visible_left) * 4),
                    (void*)(current_framebuffer_address + ((visible_top_relative + y) * window->width + visible_left_relative) * 4),
                    visible_width * 4
                );
            }
        }

        intptr_t cursor_size = 16;
        for(auto y = mouse_y; y < mouse_y + cursor_size; y += 1) {
            for(auto x = mouse_x; x < mouse_x + cursor_size; x += 1) {
                if(x >= 0 && x < (intptr_t)display_width && y >= 0 && y < (intptr_t)display_height) {
                    ((uint32_t*)display_framebuffer_address)[(size_t)y * display_width + x] = 0xFFFFFF;
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