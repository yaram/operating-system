#pragma once

#include <stddef.h>

enum struct SyscallType : size_t {
    Exit,
    RelinquishTime,
    DebugPrint,
    FindPCIEDevice,
    MapPCIEConfiguration,
    MapPCIEBar
};