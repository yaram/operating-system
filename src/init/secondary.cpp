#include <stdint.h>
#include <stddef.h>
#include "secondary.h"
#include "printf.h"
#include "syscall.h"
#include "HandmadeMath.h"

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

    size_t counter = 0;

    while(true) {
        uint32_t *framebuffer;
        if(*swap_indicator) {
            framebuffer = (uint32_t*)framebuffers_address;
        } else {
            framebuffer = (uint32_t*)(framebuffers_address + framebuffer_size);
        }

        auto time = (float)counter / 100;

        const size_t triangle_count = 1;
        hmm_vec3 triangles[triangle_count][3] {
            {
                { 100.0f + HMM_CosF(time * HMM_PI32 * 2) * 10, 120.0f - HMM_SinF(time * HMM_PI32 * 2) * 10, 0.0f },
                { 130.0f, 200.0f, 0.0f },
                { 300.0f, 150.0f, 0.0f }
            }
        };

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