#pragma once

#include <stddef.h>

enum struct SyscallType : size_t {
    Exit,
    RelinquishTime,
    DebugPrint,
    MapFreeMemory,
    MapFreeConsecutiveMemory,
    UnmapMemory,
    FindPCIEDevice,
    MapPCIEConfiguration,
    MapPCIEBar
};