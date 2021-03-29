#pragma once

enum struct CompositorConnectionResult {
    Success,
    InvalidProcessID,
    OutOfMemory
};

struct CompositorConnectionMailbox {
    bool locked;

    bool connection_requested;

    // Parameters
    size_t process_id;

    // Returns
    CompositorConnectionResult result;
    size_t mailbox_shared_memory;
    size_t ring_shared_memory;
};

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

enum struct CompositorEventType {
    KeyDown,
    KeyUp,
    MouseMove,
    FocusGained,
    FocusLost,
    CloseRequested
};

struct CompositorEvent {
    CompositorEventType type;

    size_t window_id;

    union {
        struct {
            uint16_t scancode;
        } key_down;

        struct {
            uint16_t scancode;
        } key_up;

        struct {
            intptr_t x;
            intptr_t y;

            intptr_t dx;
            intptr_t dy;
        } mouse_move;

        struct {
            intptr_t mouse_x;
            intptr_t mouse_y;
        } focus_gained;
    };
};

const size_t compositor_ring_length = 16;

struct CompositorRing {
    size_t read_head;
    size_t write_head;

    CompositorEvent entries[compositor_ring_length];
};