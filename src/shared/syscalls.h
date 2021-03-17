#pragma once

#include <stddef.h>

enum struct CreateProcessResult : size_t {
    Success,
    OutOfMemory,
    InvalidELF,
    InvalidMemoryRange
};

enum struct SyscallType : size_t {
    Exit,
    RelinquishTime,
    DebugPrint,
    MapFreeMemory,
    MapFreeConsecutiveMemory,
    UnmapMemory,
    CreateProcess,
    FindPCIEDevice,
    MapPCIEConfiguration,
    MapPCIEBar
};