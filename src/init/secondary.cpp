#include <stdint.h>
#include <stddef.h>
#include "compositor.h"
#include "secondary.h"
#include "printf.h"
#include "syscall.h"
#include "HandmadeMath.h"
#include "virtio.h"
#include "bucket_array.h"

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

extern "C" [[noreturn]] void entry(size_t process_id, void *data, size_t data_size) {
    printf("Secondary process started!\n");

    if(data == nullptr) {
        printf("Error: Missing data for secondary process");

        exit();
    }

    if(data_size != sizeof(SecondaryProcessParameters)) {
        printf("Error: Invalid data size for secondary process. Expected %zu, got %zu\n", sizeof(SecondaryProcessParameters), data_size);

        exit();
    }

    auto parameters = (SecondaryProcessParameters*)data;

    volatile CompositorMailbox *compositor_mailbox;
    {
        MapSharedMemoryParameters syscall_parameters {
            parameters->compositor_process_id,
            parameters->compositor_mailbox_shared_memory,
            sizeof(CompositorMailbox)
        };
        switch((MapSharedMemoryResult)syscall(SyscallType::MapSharedMemory, (size_t)&syscall_parameters, 0, (size_t*)&compositor_mailbox)) {
            case MapSharedMemoryResult::Success: break;

            case MapSharedMemoryResult::OutOfMemory: {
                printf("Error: Unable to map compositor mailbox shared memory: Out of memory\n");

                exit();
            } break;

            case MapSharedMemoryResult::InvalidProcessID: {
                printf("Error: Unable to map compositor mailbox shared memory: Compositor process does not exist\n");

                exit();
            } break;

            case MapSharedMemoryResult::InvalidMemoryRange: {
                printf("Error: Unable to map compositor mailbox shared memory: Invalid memory range\n");

                exit();
            } break;
        }
    }

    intptr_t window_width = 300;
    intptr_t window_height = 300;

    auto framebuffer_size = (size_t)(window_width * window_height * 4);

    struct Window {
        size_t id;

        size_t framebuffers_address;
        bool *swap_indicator;
    };

    using Windows = BucketArray<Window, 4>;

    Windows windows {};

    auto window_count = 5;

    for(auto i = 0; i < window_count; i += 1) {
        compositor_mailbox->command_type = CompositorCommandType::CreateWindow;

        auto command = &compositor_mailbox->create_window;

        command->x = i * 100;
        command->y = i * 50;
        command->width = window_width;
        command->height = window_height;

        compositor_mailbox->command_present = true;

        while(compositor_mailbox->command_present) {
            syscall(SyscallType::RelinquishTime, 0, 0);
        }

        switch(command->result) {
            case CreateWindowResult::Success: break;

            case CreateWindowResult::InvalidSize: {
                printf("Error: Unable to create window: Invalid size\n");

                exit();
            } break;

            case CreateWindowResult::OutOfMemory: {
                printf("Error: Unable to create window: Out of memory\n");

                exit();
            } break;
        }

        auto window = allocate_from_bucket_array(&windows);
        if(window == nullptr) {
            printf("Error: Unable to allocate window: Out of memory\n");

            exit();
        }

        size_t framebuffers_address;
        {
            MapSharedMemoryParameters syscall_parameters {
                parameters->compositor_process_id,
                command->framebuffers_shared_memory,
                framebuffer_size * 2
            };
            switch((MapSharedMemoryResult)syscall(SyscallType::MapSharedMemory, (size_t)&syscall_parameters, 0, &framebuffers_address)) {
                case MapSharedMemoryResult::Success: break;

                case MapSharedMemoryResult::OutOfMemory: {
                    printf("Error: Unable to map framebuffers shared memory: Out of memory\n");

                    exit();
                } break;

                case MapSharedMemoryResult::InvalidProcessID: {
                    printf("Error: Unable to map framebuffers shared memory: Compositor process does not exist\n");

                    exit();
                } break;

                case MapSharedMemoryResult::InvalidMemoryRange: {
                    printf("Error: Unable to map framebuffers shared memory: Invalid memory range\n");

                    exit();
                } break;
            }
        }

        bool *swap_indicator;
        {
            MapSharedMemoryParameters syscall_parameters {
                parameters->compositor_process_id,
                command->swap_indicator_shared_memory,
                1
            };
            switch((MapSharedMemoryResult)syscall(SyscallType::MapSharedMemory, (size_t)&syscall_parameters, 0, (size_t*)&swap_indicator)) {
                case MapSharedMemoryResult::Success: break;

                case MapSharedMemoryResult::OutOfMemory: {
                    printf("Error: Unable to swap indicator shared memory: Out of memory\n");

                    exit();
                } break;

                case MapSharedMemoryResult::InvalidProcessID: {
                    printf("Error: Unable to swap indicator shared memory: Compositor process does not exist\n");

                    exit();
                } break;

                case MapSharedMemoryResult::InvalidMemoryRange: {
                    printf("Error: Unable to swap indicator shared memory: Invalid memory range\n");

                    exit();
                } break;
            }
        }

        window->id = compositor_mailbox->create_window.id;
        window->framebuffers_address = framebuffers_address;
        window->swap_indicator = swap_indicator;
    }

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

    size_t counter = 0;

    auto moving_up = false;
    auto moving_down = false;
    auto moving_left = false;
    auto moving_right = false;

    auto mouse_x = 0;
    auto mouse_y = 0;

    hmm_vec2 position {};

    while(true) {
        size_t window_index = 0;
        for(auto window : windows) {
            // Aquire the current offscreen swapbuffer to prevent flickering (double buffering)
            auto framebuffer = (uint32_t*)(window->framebuffers_address + (size_t)!*window->swap_indicator * framebuffer_size);

            auto mouse_dx = 0;
            auto mouse_dy = 0;

            for(auto device : virtio_input_devices) {
                auto used_index = device->used_ring->idx;

                if(used_index != device->previous_used_index) {
                    for(uint16_t index = device->previous_used_index; index != used_index; index += 1) {
                        auto descriptor_index = (size_t)device->used_ring->ring[index % virtio_input_queue_descriptor_count].id;

                        auto event = (volatile virtio_input_event*)(device->buffers_address + virtio_input_buffer_size * descriptor_index);

                        switch(event->type) {
                            case 1: { // EV_KEY
                                switch(event->code) {
                                    case 17: { // KEY_W
                                        moving_up = (bool)event->value;
                                    } break;

                                    case 31: { // KEY_S
                                        moving_down = (bool)event->value;
                                    } break;

                                    case 30: { // KEY_A
                                        moving_left = (bool)event->value;
                                    } break;

                                    case 32: { // KEY_D
                                        moving_right = (bool)event->value;
                                    } break;
                                }
                            } break;

                            case 2: { // EV_REL
                                switch(event->code) {
                                    case 0: { // REL_X
                                        mouse_x += (int32_t)event->value;
                                        mouse_dx += (int32_t)event->value;
                                    } break;

                                    case 1: { // REL_Y
                                        mouse_y += (int32_t)event->value;
                                        mouse_dy += (int32_t)event->value;
                                    } break;
                                }
                            } break;
                        }

                        device->available_ring->ring[device->available_ring->idx % virtio_input_queue_descriptor_count] = descriptor_index;

                        device->available_ring->idx += 1;
                    }

                    device->previous_used_index = device->used_ring->idx;
                }
            }

            auto time = (float)counter / 100;

            auto speed = 1.0f;

            if(moving_up) {
                position.Y -= speed;
            }

            if(moving_down) {
                position.Y += speed;
            }

            if(moving_left) {
                position.X -= speed;
            }

            if(moving_right) {
                position.X += speed;
            }

            position.X += mouse_dx;
            position.Y += mouse_dy;

            const size_t triangle_count = 1;
            hmm_vec3 triangles[triangle_count][3] {
                {
                    { 20.0f + HMM_CosF(time * HMM_PI32 * 2) * 10, 20.0f + HMM_SinF(time * HMM_PI32 * 2) * 10, 0.0f },
                    { 30.0f, 100.0f, 0.0f },
                    { 200.0f, 50.0f, 0.0f }
                }
            };

            for(size_t i = 0; i < triangle_count; i += 1) {
                triangles[i][0].XY += position;
                triangles[i][1].XY += position;
                triangles[i][2].XY += position;
            }

            memset(framebuffer, 0x80, framebuffer_size);

            auto shade = window_index / (float)(window_count - 1);

            auto shade_int = (uint8_t)(0xFF * shade);

            auto color = 0xFF | shade_int << 8 | shade_int << 16;

            for(size_t i = 0; i < triangle_count; i += 1) {
                auto first = triangles[i][0];
                auto second = triangles[i][1];
                auto third = triangles[i][2];

                if(first.Y > second.Y) {
                    auto temp = first;
                    first = second;
                    second = temp;
                }

                if(first.Y > third.Y) {
                    auto temp = first;
                    first = third;
                    third = temp;
                }

                if(second.Y > third.Y) {
                    auto temp = second;
                    second = third;
                    third = temp;
                }

                auto long_mid_x = HMM_Lerp(first.X, ((float)second.Y - first.Y) / (third.Y - first.Y), third.X);
                auto short_mid_x = second.X;

                auto first_y_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(first.Y, 0), window_height);
                auto second_y_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(second.Y, 0), window_height);
                auto third_y_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(third.Y, 0), window_height);

                if(long_mid_x < short_mid_x) {
                    for(size_t y = first_y_pixel; y < second_y_pixel; y += 1) {
                        auto long_progress = ((float)y - first.Y) / (third.Y - first.Y);

                        auto long_x = HMM_Lerp(first.X, long_progress, third.X);

                        auto long_x_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(long_x, 0), window_width);

                        auto short_progress = ((float)y - first.Y) / (second.Y - first.Y);

                        auto short_x = HMM_Lerp(first.X, short_progress, second.X);

                        auto short_x_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(short_x, 0), window_width);

                        for(size_t x = long_x_pixel; x < short_x_pixel; x += 1) {
                            framebuffer[y * window_width + x] = color;
                        }
                    }

                    for(size_t y = second_y_pixel; y < third_y_pixel; y += 1) {
                        auto long_progress = ((float)y - first.Y) / (third.Y - first.Y);

                        auto long_x = HMM_Lerp(first.X, long_progress, third.X);

                        auto long_x_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(long_x, 0), window_width);

                        auto short_progress = ((float)y - second.Y) / (third.Y - second.Y);

                        auto short_x = HMM_Lerp(second.X, short_progress, third.X);

                        auto short_x_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(short_x, 0), window_width);

                        for(size_t x = long_x_pixel; x < short_x_pixel; x += 1) {
                            framebuffer[y * window_width + x] = color;
                        }
                    }
                } else {
                    for(size_t y = first_y_pixel; y < second_y_pixel; y += 1) {
                        auto long_progress = ((float)y - first.Y) / (third.Y - first.Y);

                        auto long_x = HMM_Lerp(first.X, long_progress, third.X);

                        auto long_x_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(long_x, 0), window_width);

                        auto short_progress = ((float)y - first.Y) / (second.Y - first.Y);

                        auto short_x = HMM_Lerp(first.X, short_progress, second.X);

                        auto short_x_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(short_x, 0), window_width);

                        for(size_t x = short_x_pixel; x < long_x_pixel; x += 1) {
                            framebuffer[y * window_width + x] = color;
                        }
                    }

                    for(size_t y = second_y_pixel; y < third_y_pixel; y += 1) {
                        auto long_progress = ((float)y - first.Y) / (third.Y - first.Y);

                        auto long_x = HMM_Lerp(first.X, long_progress, third.X);

                        auto long_x_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(long_x, 0), window_width);

                        auto short_progress = ((float)y - second.Y) / (third.Y - second.Y);

                        auto short_x = HMM_Lerp(second.X, short_progress, third.X);

                        auto short_x_pixel = (size_t)HMM_MIN((intptr_t)HMM_MAX(short_x, 0), window_width);

                        for(size_t x = short_x_pixel; x < long_x_pixel; x += 1) {
                            framebuffer[y * window_width + x] = color;
                        }
                    }
                }
            }

            *window->swap_indicator = !*window->swap_indicator;

            window_index += 1;
        }

        counter += 1;

    }

    exit();
}