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

    volatile CompositorRing *compositor_ring;
    {
        MapSharedMemoryParameters syscall_parameters {
            parameters->compositor_process_id,
            parameters->compositor_ring_shared_memory,
            sizeof(CompositorRing)
        };
        switch((MapSharedMemoryResult)syscall(SyscallType::MapSharedMemory, (size_t)&syscall_parameters, 0, (size_t*)&compositor_ring)) {
            case MapSharedMemoryResult::Success: break;

            case MapSharedMemoryResult::OutOfMemory: {
                printf("Error: Unable to map compositor ring shared memory: Out of memory\n");

                exit();
            } break;

            case MapSharedMemoryResult::InvalidProcessID: {
                printf("Error: Unable to map compositor ring shared memory: Compositor process does not exist\n");

                exit();
            } break;

            case MapSharedMemoryResult::InvalidMemoryRange: {
                printf("Error: Unable to map compositor ring shared memory: Invalid memory range\n");

                exit();
            } break;
        }
    }

    intptr_t window_width = 300;
    intptr_t window_height = 300;

    auto framebuffer_size = (size_t)(window_width * window_height * 4);

    struct Window {
        size_t id;

        bool moving_up;
        bool moving_down;
        bool moving_left;
        bool moving_right;

        hmm_vec2 position;

        hmm_vec3 color;

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

        auto shade = i / (float)(window_count - 1);

        window->id = compositor_mailbox->create_window.id;
        window->color = HMM_Vec3(1, shade, shade);
        window->framebuffers_address = framebuffers_address;
        window->swap_indicator = swap_indicator;
    }

    size_t counter = 0;

    while(true) {
        auto mouse_dx = 0;
        auto mouse_dy = 0;

        while(true) {
            auto next_read_head = (compositor_ring->read_head + 1) % compositor_ring_length;

            if(next_read_head == compositor_ring->write_head) {
                break;
            }

            auto entry = &compositor_ring->entries[next_read_head];

            Window *window;
            for(auto the_window : windows) {
                if(entry->window_id == the_window->id) {
                    window = the_window;
                }
            }

            switch(entry->type) {
                case CompositorEventType::KeyDown: {
                    switch(entry->key_down.scancode) {
                        case 17: { // KEY_W
                            window->moving_up = true;
                        } break;

                        case 31: { // KEY_S
                            window->moving_down = true;
                        } break;

                        case 30: { // KEY_A
                            window->moving_left = true;
                        } break;

                        case 32: { // KEY_D
                            window->moving_right = true;
                        } break;
                    }
                } break;

                case CompositorEventType::KeyUp: {
                    switch(entry->key_down.scancode) {
                        case 17: { // KEY_W
                            window->moving_up = false;
                        } break;

                        case 31: { // KEY_S
                            window->moving_down = false;
                        } break;

                        case 30: { // KEY_A
                            window->moving_left = false;
                        } break;

                        case 32: { // KEY_D
                            window->moving_right = false;
                        } break;
                    }
                } break;

                case CompositorEventType::MouseMove: {
                    window->position.X += entry->mouse_move.dx;
                    window->position.Y += entry->mouse_move.dy;
                } break;

                case CompositorEventType::FocusLost: {
                    window->moving_up = false;
                    window->moving_down = false;
                    window->moving_left = false;
                    window->moving_right = false;
                } break;
            }

            compositor_ring->read_head = next_read_head;
        }

        for(auto window : windows) {
            // Aquire the current offscreen swapbuffer to prevent flickering (double buffering)
            auto framebuffer = (uint32_t*)(window->framebuffers_address + (size_t)!*window->swap_indicator * framebuffer_size);

            auto time = (float)counter / 100;

            auto speed = 1.0f;

            if(window->moving_up) {
                window->position.Y -= speed;
            }

            if(window->moving_down) {
                window->position.Y += speed;
            }

            if(window->moving_left) {
                window->position.X -= speed;
            }

            if(window->moving_right) {
                window->position.X += speed;
            }

            window->position.X += mouse_dx;
            window->position.Y += mouse_dy;

            const size_t triangle_count = 1;
            hmm_vec3 triangles[triangle_count][3] {
                {
                    { 20.0f + HMM_CosF(time * HMM_PI32 * 2) * 10, 20.0f + HMM_SinF(time * HMM_PI32 * 2) * 10, 0.0f },
                    { 30.0f, 100.0f, 0.0f },
                    { 200.0f, 50.0f, 0.0f }
                }
            };

            for(size_t i = 0; i < triangle_count; i += 1) {
                triangles[i][0].XY += window->position;
                triangles[i][1].XY += window->position;
                triangles[i][2].XY += window->position;
            }

            memset(framebuffer, 0x80, framebuffer_size);

            auto red = (uint8_t)(0xFF * HMM_MIN(HMM_MAX(window->color.R, 0), 1));
            auto green = (uint8_t)(0xFF * HMM_MIN(HMM_MAX(window->color.G, 0), 1));
            auto blue = (uint8_t)(0xFF * HMM_MIN(HMM_MAX(window->color.B, 0), 1));

            auto color = (uint32_t)red | (uint32_t)green << 8 | (uint32_t)blue << 16;

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
        }

        counter += 1;

    }

    exit();
}