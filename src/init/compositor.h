#pragma once

enum struct CompositorCommandType {
    CreateWindow,
    DestroyWindow
};

enum struct CreateWindowResult {
    Success,
    InvalidSize,
    OutOfMemory
};

struct CompositorMailbox {
    bool command_present;

    CompositorCommandType command_type;

    union {
        struct {
            // Parameters
            intptr_t x;
            intptr_t y;
            intptr_t width;
            intptr_t height;

            // Returns
            CreateWindowResult result;
            size_t id;
            size_t framebuffers_shared_memory;
            size_t swap_indicator_shared_memory;
        } create_window;

        struct {
            // Parameters
            size_t id;
        } destroy_window;
    };
};