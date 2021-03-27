#pragma once

struct SecondaryProcessParameters {
    size_t compositor_process_id;

    size_t compositor_mailbox_shared_memory;

    size_t compositor_ring_shared_memory;
};