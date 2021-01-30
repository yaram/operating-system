#pragma once

const size_t kernel_memory_start = 0;
const size_t kernel_memory_end = 0x800000;

struct MemoryMapEntry {
    void* address;
    size_t length;
    bool available;
};