#pragma once

#include <stddef.h>

struct MapSharedMemoryParameters {
    size_t process_id;
    size_t address;
    size_t size;
};

enum struct MapSharedMemoryResult : size_t {
    Success,
    OutOfMemory,
    InvalidProcessID,
    InvalidMemoryRange
};

struct CreateProcessParameters {
    void *elf_binary;
    size_t elf_binary_size;

    void *data;
    size_t data_size;
};

enum struct CreateProcessResult : size_t {
    Success,
    OutOfMemory,
    InvalidELF,
    InvalidMemoryRange
};

struct FindPCIEDeviceParameters {
    uint16_t vendor_id;
    uint16_t device_id;

    uint8_t class_code;
    uint8_t subclass;
    uint8_t interface;

    bool require_vendor_id;
    bool require_device_id;

    bool require_class_code;
    bool require_subclass;
    bool require_interface;
};

enum struct FindPCIEDeviceResult : size_t {
    Success,
    NotFound,
    OutOfMemory,
    InvalidMemoryRange
};

static_assert(sizeof(bool) == 1, "Boolean (bool) type is not the expected size of 1 byte");

enum struct SyscallType : size_t {
    Exit,
    RelinquishTime,
    DebugPrint,
    MapFreeMemory,
    MapFreeConsecutiveMemory,
    CreateSharedMemory,
    MapSharedMemory,
    UnmapMemory,
    CreateProcess,
    FindPCIEDevice,
    MapPCIEConfiguration,
    MapPCIEBar
};