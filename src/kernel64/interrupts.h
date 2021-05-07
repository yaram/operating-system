#pragma once

#include <stdint.h>

struct InterruptStackFrame {
    uint64_t error_code;
    void *instruction_pointer;
    uint64_t code_segment;
    uint64_t cpu_flags;
    void *stack_pointer;
    uint64_t stack_segment;
};

extern "C" uint8_t exception_handler_thunk_0[];
extern "C" uint8_t exception_handler_thunk_1[];
extern "C" uint8_t exception_handler_thunk_2[];
extern "C" uint8_t exception_handler_thunk_3[];
extern "C" uint8_t exception_handler_thunk_4[];
extern "C" uint8_t exception_handler_thunk_5[];
extern "C" uint8_t exception_handler_thunk_6[];
extern "C" uint8_t exception_handler_thunk_7[];
extern "C" uint8_t exception_handler_thunk_8[];
extern "C" uint8_t exception_handler_thunk_10[];
extern "C" uint8_t exception_handler_thunk_11[];
extern "C" uint8_t exception_handler_thunk_12[];
extern "C" uint8_t exception_handler_thunk_13[];
extern "C" uint8_t exception_handler_thunk_14[];
extern "C" uint8_t exception_handler_thunk_15[];
extern "C" uint8_t exception_handler_thunk_16[];
extern "C" uint8_t exception_handler_thunk_17[];
extern "C" uint8_t exception_handler_thunk_18[];
extern "C" uint8_t exception_handler_thunk_19[];
extern "C" uint8_t exception_handler_thunk_20[];
extern "C" uint8_t exception_handler_thunk_30[];

extern "C" uint8_t preempt_timer_handler_thunk[];
extern "C" uint8_t spurious_interrupt_handler_thunk[];
extern "C" uint8_t kernel_page_tables_update_handler_thunk[];