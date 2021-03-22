#pragma once

struct SecondaryProcessParameters {
    size_t compositor_process_id;

    size_t framebuffers_shared_memory_address;
    size_t framebuffer_width;
    size_t framebuffer_height;

    size_t swap_indicator_shared_memory_address;
};