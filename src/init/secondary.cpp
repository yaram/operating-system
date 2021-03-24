#include <stdint.h>
#include <stddef.h>
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
static bool find_or_allocate_unoccupied_bucket_slot(
    BucketArray<T, N> *bucket_array,
    BucketArrayIterator<T, N> *result_iterator
) {
    auto found_iterator = find_unoccupied_bucket_slot(bucket_array);

    if(found_iterator.current_bucket == nullptr) {
        auto new_bucket = (Bucket<T, N>*)syscall(SyscallType::MapFreeMemory, sizeof(Bucket<T, N>), 0);
        if(new_bucket == nullptr) {
            return false;
        }

        {
            auto current_bucket = &bucket_array->first_bucket;
            while(current_bucket->next != nullptr) {
                current_bucket = current_bucket->next;
            }

            current_bucket->next = new_bucket;
        }

        *result_iterator = {
            new_bucket,
            0
        };
        return true;
    } else {
        *result_iterator = found_iterator;
        return true;
    }
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

    auto framebuffer_size = parameters->framebuffer_width * parameters->framebuffer_height * 4;

    size_t framebuffers_address;
    {
        MapSharedMemoryParameters syscall_parameters {
            parameters->compositor_process_id,
            parameters->framebuffers_shared_memory_address,
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
            parameters->swap_indicator_shared_memory_address,
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

        VirtIOInputDevices::Iterator device_iterator;
        if(!find_or_allocate_unoccupied_bucket_slot(&virtio_input_devices, &device_iterator)) {
            printf("Error: Out of memory\n");

            exit();
        }

        device_iterator.current_bucket->occupied[device_iterator.current_sub_index] = true;

        auto device = *device_iterator;
        memset(device, 0, sizeof(VirtIOInputDevice));

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
        uint32_t *framebuffer;
        if(*swap_indicator) {
            framebuffer = (uint32_t*)framebuffers_address;
        } else {
            framebuffer = (uint32_t*)(framebuffers_address + framebuffer_size);
        }

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

            auto first_y_pixel = HMM_MIN((size_t)HMM_MAX(first.Y, 0), parameters->framebuffer_height);
            auto second_y_pixel = HMM_MIN((size_t)HMM_MAX(second.Y, 0), parameters->framebuffer_height);
            auto third_y_pixel = HMM_MIN((size_t)HMM_MAX(third.Y, 0), parameters->framebuffer_height);

            if(long_mid_x < short_mid_x) {
                for(size_t y = first_y_pixel; y < second_y_pixel; y += 1) {
                    auto long_progress = ((float)y - first.Y) / (third.Y - first.Y);

                    auto long_x = HMM_Lerp(first.X, long_progress, third.X);

                    auto long_x_pixel = HMM_MIN((size_t)HMM_MAX(long_x, 0), parameters->framebuffer_width);

                    auto short_progress = ((float)y - first.Y) / (second.Y - first.Y);

                    auto short_x = HMM_Lerp(first.X, short_progress, second.X);

                    auto short_x_pixel = HMM_MIN((size_t)HMM_MAX(short_x, 0), parameters->framebuffer_width);

                    for(size_t x = long_x_pixel; x < short_x_pixel; x += 1) {
                        framebuffer[y * parameters->framebuffer_width + x] = 0xFFFFFF;
                    }
                }

                for(size_t y = second_y_pixel; y < third_y_pixel; y += 1) {
                    auto long_progress = ((float)y - first.Y) / (third.Y - first.Y);

                    auto long_x = HMM_Lerp(first.X, long_progress, third.X);

                    auto long_x_pixel = HMM_MIN((size_t)HMM_MAX(long_x, 0), parameters->framebuffer_width);

                    auto short_progress = ((float)y - second.Y) / (third.Y - second.Y);

                    auto short_x = HMM_Lerp(second.X, short_progress, third.X);

                    auto short_x_pixel = HMM_MIN((size_t)HMM_MAX(short_x, 0), parameters->framebuffer_width);

                    for(size_t x = long_x_pixel; x < short_x_pixel; x += 1) {
                        framebuffer[y * parameters->framebuffer_width + x] = 0xFFFFFF;
                    }
                }
            } else {
                for(size_t y = first_y_pixel; y < second_y_pixel; y += 1) {
                    auto long_progress = ((float)y - first.Y) / (third.Y - first.Y);

                    auto long_x = HMM_Lerp(first.X, long_progress, third.X);

                    auto long_x_pixel = HMM_MIN((size_t)HMM_MAX(long_x, 0), parameters->framebuffer_width);

                    auto short_progress = ((float)y - first.Y) / (second.Y - first.Y);

                    auto short_x = HMM_Lerp(first.X, short_progress, second.X);

                    auto short_x_pixel = HMM_MIN((size_t)HMM_MAX(short_x, 0), parameters->framebuffer_width);

                    for(size_t x = short_x_pixel; x < long_x_pixel; x += 1) {
                        framebuffer[y * parameters->framebuffer_width + x] = 0xFFFFFF;
                    }
                }

                for(size_t y = second_y_pixel; y < third_y_pixel; y += 1) {
                    auto long_progress = ((float)y - first.Y) / (third.Y - first.Y);

                    auto long_x = HMM_Lerp(first.X, long_progress, third.X);

                    auto long_x_pixel = HMM_MIN((size_t)HMM_MAX(long_x, 0), parameters->framebuffer_width);

                    auto short_progress = ((float)y - second.Y) / (third.Y - second.Y);

                    auto short_x = HMM_Lerp(second.X, short_progress, third.X);

                    auto short_x_pixel = HMM_MIN((size_t)HMM_MAX(short_x, 0), parameters->framebuffer_width);

                    for(size_t x = short_x_pixel; x < long_x_pixel; x += 1) {
                        framebuffer[y * parameters->framebuffer_width + x] = 0xFFFFFF;
                    }
                }
            }
        }

        counter += 1;

        *swap_indicator = !*swap_indicator;
    }
}