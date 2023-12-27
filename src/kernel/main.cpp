#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "acpi.h"
}
#include "console.h"
#include "heap.h"
#include "paging.h"
#include "process.h"
#include "memory.h"
#include "bucket_array.h"
#include "bucket_array_kernel.h"
#include "syscalls.h"
#include "pcie.h"
#include "halt.h"
#include "array.h"
#include "multiprocessing.h"
#include "io.h"
#include "kernel_extern.h"

#define do_ranges_intersect(a_start, a_end, b_start, b_end) (!((a_end) <= (b_start) || (b_end) <= (a_start)))

static inline void acpi_call(ACPI_STATUS status, const char *message) {
    if(status != AE_OK) {
        printf("ERROR: %s (0x%X)\n", message, status);

        halt();
    }
}

static inline void print_time(const char *name) {
    uint32_t time_low;
    uint32_t time_high;
    asm volatile(
        "cpuid\n" // Using CPUID instruction to serialize instruction execution
        "rdtsc"
        : "=a"(time_low), "=d"(time_high)
        :
        : "ebx", "ecx"
    );

    auto time = (size_t)time_low | (size_t)time_high << 32;

    printf("Time at %s: %zu\n", name, time);
}

struct MCFGTable {
    ACPI_TABLE_MCFG preamble;

    ACPI_MCFG_ALLOCATION allocations[0];
};

struct MADTTable {
    ACPI_TABLE_MADT preamble;

    ACPI_SUBTABLE_HEADER entries[0];
};

struct __attribute__((packed)) GDTDescriptor {
    uint16_t limit;
    uint64_t base;
};

struct __attribute__((packed)) IDTEntry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t interrupt_stack_table: 3;
    uint8_t ignored: 5;
    uint8_t type: 4;
    bool storage_segment: 1;
    uint8_t privilege_level: 2;
    bool present: 1;
    uint64_t offset_high: 48;
    uint32_t reserved;
};

struct __attribute__((packed)) IDTDescriptor {
    uint16_t limit;
    uint64_t base;
};

size_t global_acpi_table_physical_address;

static size_t global_processor_count;
static size_t global_processor_area_count;
static size_t global_processor_areas_physical_address;
static ProcessorArea *global_processor_areas;

const uint32_t preempt_time = 0x100000;

const auto kernel_pd_start = kernel_pages_start / page_table_length;
const auto kernel_pd_end = divide_round_up(kernel_pages_end, page_table_length);
const auto kernel_pd_count = kernel_pd_end - kernel_pd_start;

const auto kernel_pdp_start = kernel_pd_start / page_table_length;
const auto kernel_pdp_end = divide_round_up(kernel_pd_end, page_table_length);
const auto kernel_pdp_count = kernel_pdp_end - kernel_pdp_start;

const auto kernel_pml4_start = kernel_pdp_start / page_table_length;
const auto kernel_pml4_end = divide_round_up(kernel_pdp_end, page_table_length);
const auto kernel_pml4_count = kernel_pml4_end - kernel_pml4_start;

__attribute__((aligned(page_size)))
PageTableEntry kernel_pml4_table[page_table_length] {};

__attribute__((aligned(page_size)))
PageTableEntry kernel_pdp_tables[kernel_pml4_count][page_table_length] {};

__attribute__((aligned(page_size)))
PageTableEntry kernel_pd_tables[kernel_pdp_count][page_table_length] {};

__attribute__((aligned(page_size)))
PageTableEntry kernel_page_tables[kernel_pd_count][page_table_length] {};

extern uint8_t multiprocessor_binary[];
extern uint8_t multiprocessor_binary_end[];

extern uint8_t embedded_init_binary[];
extern uint8_t embedded_init_binary_end[];

static volatile bool global_additional_processor_initialized_flag = false;
volatile bool global_all_processors_initialized = false;

Array<uint8_t> global_bitmap;

Processes global_processes {};

[[noreturn]] static inline void unreachable_implementation(const char *file, unsigned int line) {
    printf("UNREACHABLE CODE EXECUTED at %s:%u\n", file, line);

    halt();
}

#define unreachable() unreachable_implementation(__FILE__, __LINE__)

// APIC timer must be disabled or at 0 when this function is called, and there must be no pending APIC timer interrupts
[[noreturn]] static void enter_next_process(
    ProcessorArea *processor_area,
    Array<uint8_t> bitmap,
    Processes *processes
) {
    Process *process;
    ProcessThread *thread;
    if(processor_area->current_process_iterator.current_bucket != nullptr) {
        ++processor_area->current_thread_iterator;

        process = *processor_area->current_process_iterator;

        if(process->is_ready) {
            while(true) {
                if(processor_area->current_thread_iterator.current_bucket == nullptr) {
                    break;
                }

                thread = *processor_area->current_thread_iterator;

                if(thread->is_ready && compare_and_swap(&thread->is_resident, false, true)) {
                    break;
                }

                ++processor_area->current_thread_iterator;
            }
        }
    }

    if(processor_area->current_thread_iterator.current_bucket == nullptr) {
        ++processor_area->current_process_iterator;

        while(true) {
            if(processor_area->current_process_iterator.current_bucket == nullptr) {
                break;
            }

            process = *processor_area->current_process_iterator;
            processor_area->current_thread_iterator = begin(process->threads);

            if(process->is_ready) {
                while(true) {
                    if(processor_area->current_thread_iterator.current_bucket == nullptr) {
                        break;
                    }

                    thread = *processor_area->current_thread_iterator;

                    if(thread->is_ready && compare_and_swap(&thread->is_resident, false, true)) {
                        break;
                    }

                    ++processor_area->current_thread_iterator;
                }

                if(processor_area->current_thread_iterator.current_bucket != nullptr) {
                    break;
                }
            }

            ++processor_area->current_process_iterator;
        }
    }

    /// Second iteration from start of process list
    if(processor_area->current_process_iterator.current_bucket == nullptr) {
        processor_area->current_process_iterator = begin(*processes);

        while(true) {
            if(processor_area->current_process_iterator.current_bucket == nullptr) {
                // Disable interrupts until stack is correctly setup for interrupt safety
                asm volatile(
                    "cli"
                );

                // These values may not be reset under certain conditions, so reset them here while interrupts are disabled
                processor_area->in_syscall_or_user_exception = false;
                processor_area->preempt_during_syscall_or_user_exception = false;

                // Set timer value
                processor_area->apic_registers->timer_initial_count.value = preempt_time;

                // Enable APIC timer
                processor_area->apic_registers->lvt_timer.value &= ~(1 << 16);

                // Halt current processor until next timer interval, also reset stack to top to prevent stack overflow
                asm volatile(
                    "mov %0, %%rsp\n"
                    "sti\n"
                    ".loop:\n"
                    "hlt\n"
                    "jmp .loop"
                    :
                    : "r"((size_t)&processor_area->stack + processor_stack_size)
                );

                unreachable();
            }

            process = *processor_area->current_process_iterator;
            processor_area->current_thread_iterator = begin(process->threads);

            if(process->is_ready) {
                while(true) {
                    if(processor_area->current_thread_iterator.current_bucket == nullptr) {
                        break;
                    }
                    
                    thread = *processor_area->current_thread_iterator;

                    if(thread->is_ready && compare_and_swap(&thread->is_resident, false, true)) {
                        break;
                    }

                    ++processor_area->current_thread_iterator;
                }

                if(processor_area->current_thread_iterator.current_bucket != nullptr) {
                    break;
                }
            }

            ++processor_area->current_process_iterator;
        }
    }

    auto processor_id = get_processor_id();

    auto processor_area_user_address = user_processor_areas_memory_start + processor_id * sizeof(ProcessorArea);

    auto stack_kernel_address = (size_t)&processor_area->stack;
    auto stack_user_address = processor_area_user_address + offsetof(ProcessorArea, stack);

    GDTDescriptor gdt_descriptor {
        (uint16_t)(gdt_size * sizeof(GDTEntry) - 1),
        processor_area_user_address + offsetof(ProcessorArea, gdt_entries)
    };

    thread->resident_processor_id = processor_id;

    // Must be a full copy on the kernel stack, so that the user-page-table user_enter_thunk can access it
    auto stack_frame_copy = thread->frame;

    auto stack_frame_copy_kernel_address = (size_t)&stack_frame_copy;

    auto stack_frame_copy_relative_address = stack_frame_copy_kernel_address - stack_kernel_address;

    auto stack_frame_copy_user_address = stack_user_address + stack_frame_copy_relative_address;

    // Disable interrupts until process entry for interrupt safety
    asm volatile(
        "cli"
    );

    // These values may not be reset under certain conditions, so reset them here while interrupts are disabled
    processor_area->in_syscall_or_user_exception = false;
    processor_area->preempt_during_syscall_or_user_exception = false;

    // Set timer value
    processor_area->apic_registers->timer_initial_count.value = preempt_time;

    // Enable APIC timer
    processor_area->apic_registers->lvt_timer.value &= ~(1 << 16);

    asm volatile(
        // Load GDT
        "lgdtq (%0)\n"

        // Switch to user page tables
        "mov %1, %%cr3\n"

        // Jump to thunk for entering user mode
        "jmp user_enter_thunk"
        :
        : "r"(&gdt_descriptor), "r"(process->pml4_table_physical_address), "D"(stack_frame_copy_user_address)
    );

    unreachable();
}

// Basically using always_inline as a type-safe macro
static __attribute__((always_inline)) void continue_in_function_return(ThreadStackFrame *stack_frame, void (*function_continued)(ThreadStackFrame*)) {
    size_t current_pml4_table;
    asm volatile(
        "mov %%cr3, %0"
        : "=r"(current_pml4_table)
    );

    if(current_pml4_table == (size_t)&kernel_pml4_table) {
        function_continued(stack_frame);
    } else {
        auto processor_id = get_processor_id();

        auto processor_area_offset = processor_id * sizeof(ProcessorArea);

        auto frame_user_address = (size_t)stack_frame;

        auto processor_area_kernel_address = (size_t)global_processor_areas + processor_area_offset;
        auto processor_area_user_address = user_processor_areas_memory_start + processor_area_offset;

        auto stack_offset = offsetof(ProcessorArea, stack);

        auto stack_kernel_address = processor_area_kernel_address + stack_offset;
        auto stack_user_address = processor_area_user_address + stack_offset;

        GDTDescriptor kernel_gdt_descriptor {
            (uint16_t)(gdt_size * sizeof(GDTEntry) - 1),
            processor_area_kernel_address + offsetof(ProcessorArea, gdt_entries)
        };

        auto kernel_gdt_descriptor_user_address = (size_t)&kernel_gdt_descriptor;

        GDTDescriptor user_gdt_descriptor {
            (uint16_t)(gdt_size * sizeof(GDTEntry) - 1),
            processor_area_user_address + offsetof(ProcessorArea, gdt_entries)
        };

        auto user_gdt_descriptor_user_address = (size_t)&user_gdt_descriptor;

        auto kernel_pml4_table_address = (size_t)&kernel_pml4_table;

        auto function_continued_address = (size_t)function_continued;

        asm volatile(
            // Save needed info to stack
            "push %0\n"
            "push %1\n"
            "mov %%cr3, %%rax\n"
            "push %%rax\n"
            "push %5\n"

            // Disable interrupts for interrupt safety / atomicity
            "pushf\n"
            "cli\n"

            // Switch to kernel-mapped processor stack
            "sub %0, %%rsp\n"
            "add %1, %%rsp\n"

            // Load kernel-space GDT address into the GDTR
            "lgdt (%4)\n"

            // Switch to kernel page table
            "mov %2, %%cr3\n"

            // Re-enable interrupts
            "popf\n"

            // Calculate kernel stack frame address
            "sub %0, %%rdi\n"
            "add %1, %%rdi\n"

            // Align the stack to 16 bytes, push alignment offset to stack
            "mov %%rsp, %%rax\n"
            "and $0xF, %%rax\n"
            "and $~0xF, %%rsp\n"
            "push %%rax\n"
            "sub $8, %%rsp\n"

            // Call continued function
            "call *%3\n"

            // Restore original stack by adding back alignment offset
            "add $8, %%rsp\n"
            "pop %%rax\n"
            "add %%rax, %%rsp\n"

            // Restore saved info
            "pop %5\n"
            "pop %%rax\n"
            "pop %1\n"
            "pop %0\n"

            // Disable interrupts for interrupt safety / atomicity
            "pushf\n"
            "cli\n"

            // Switch back to user page table
            "mov %%rax, %%cr3\n"

            // Load user-space GDT address into the GDTR
            "lgdt (%5)\n"

            // Switch back to user-mapped processor stack
            "sub %1, %%rsp\n"
            "add %0, %%rsp\n"

            // Re-enable interrupts
            "popf"

            // Crazy register binding stuff with clobbers correctly specified
            : "=r"(stack_user_address), "=r"(stack_kernel_address), "=r"(kernel_pml4_table_address), "=r"(function_continued_address), "=r"(kernel_gdt_descriptor_user_address), "=r"(user_gdt_descriptor_user_address), "=D"(frame_user_address)
            : "0"(stack_user_address), "1"(stack_kernel_address), "2"(kernel_pml4_table_address), "3"(function_continued_address), "4"(kernel_gdt_descriptor_user_address), "5"(user_gdt_descriptor_user_address), "D"(frame_user_address)
            : "rax", "r10", "r11"
        );
    }
}

[[noreturn]] static __attribute__((always_inline)) void continue_in_function(const ThreadStackFrame *stack_frame, void (*function_continued)(const ThreadStackFrame*)) {
    size_t current_pml4_table;
    asm volatile(
        "mov %%cr3, %0"
        : "=r"(current_pml4_table)
    );

    if(current_pml4_table == (size_t)&kernel_pml4_table) {
        function_continued(stack_frame);
    } else {
        auto processor_id = get_processor_id();

        auto processor_area_offset = processor_id * sizeof(ProcessorArea);

        auto processor_area_kernel_address = (size_t)global_processor_areas + processor_area_offset;
        auto processor_area_user_address = user_processor_areas_memory_start + processor_area_offset;

        auto stack_offset = offsetof(ProcessorArea, stack);

        auto stack_kernel_address = processor_area_kernel_address + stack_offset;
        auto stack_user_address = processor_area_user_address + stack_offset;

        GDTDescriptor gdt_descriptor {
            (uint16_t)(gdt_size * sizeof(GDTEntry) - 1),
            processor_area_kernel_address + offsetof(ProcessorArea, gdt_entries)
        };

        asm volatile(
            // Align the stack to 16 bytes
            "and $~0xF, %%rsp\n"

            // Disable interrupts for interrupt safety / atomicity
            "pushf\n"
            "cli\n"

            // Switch to kernel-mapped processor stack
            "sub %0, %%rsp\n"
            "add %1, %%rsp\n"

            // Calculate kernel stack frame address
            "sub %0, %%rdi\n"
            "add %1, %%rdi\n"

            // Load kernel-space GDT address into the GDTR
            "lgdt (%4)\n"

            // Switch to kernel page table
            "mov %2, %%cr3\n"

            // Re-enable interrupts
            "popf\n"

            // Call continued function
            "call *%3"

            :
            : "r"(stack_user_address), "r"(stack_kernel_address), "r"(&kernel_pml4_table), "r"(function_continued), "r"(&gdt_descriptor), "D"(stack_frame)
        );
    }

    // In case the continued function returns
    unreachable();
}

[[noreturn]] void user_exception_handler_continued(const ThreadStackFrame *frame) {
    auto processor_id = get_processor_id();
    auto processor_area = &global_processor_areas[processor_id];

    if(processor_area->current_process_iterator.current_bucket != nullptr) {
        auto process = *processor_area->current_process_iterator;

        printf(" in process %zu (processor %u)\n", process->id, processor_id);

        auto instruction_pointer = (size_t)frame->interrupt_frame.instruction_pointer;

        for(auto section : process->debug_code_sections) {
            if(instruction_pointer >= section->memory_start && instruction_pointer < section->memory_start + section->size) {
                printf("Section %.*s, offset %zX\n", (int)section->name_length, section->name_buffer, instruction_pointer - section->memory_start);

                break;
            }
        }

        destroy_process(processor_area->current_process_iterator, global_bitmap);

        enter_next_process(processor_area, global_bitmap, &global_processes);
    } else {
        printf(" in kernel (processor %u)\n", processor_id);

        halt();
    }
}

extern "C" [[noreturn]] void exception_handler(size_t index, const ThreadStackFrame *frame) {
    printf("EXCEPTION 0x%X(0x%X) AT %p", index, frame->interrupt_frame.error_code, frame->interrupt_frame.instruction_pointer);

    if(index == 0x0E) { // Page Fault
        size_t fault_address;
        asm volatile(
            "mov %%cr2, %0"
            : "=r"(fault_address)
        );

        printf(" ACCESSING %p", (void*)fault_address);
    }

    if(frame->interrupt_frame.code_segment != 0x08) {
        auto user_processor_area = (ProcessorArea*)(user_processor_areas_memory_start + get_processor_id() * sizeof(ProcessorArea));

        // Disable APIC timer and clear any pending interrupt

        user_processor_area->in_syscall_or_user_exception = true;

        // Disable APIC timer
        user_processor_area->apic_registers->lvt_timer.value |= 1 << 16;

        // Re-enable interrupts
        asm volatile(
            "sti"
        );

        user_processor_area->in_syscall_or_user_exception = false;
        user_processor_area->preempt_during_syscall_or_user_exception = false;

        continue_in_function(frame, &user_exception_handler_continued);
    } else {
        printf(" in kernel (processor %u)\n", get_processor_id());

        halt();
    }
}

[[noreturn]] void preempt_timer_handler_continued(const ThreadStackFrame *frame) {
    auto processor_area = &global_processor_areas[get_processor_id()];

    // Send the End of Interrupt signal
    processor_area->apic_registers->end_of_interrupt.value = 0;

    if(processor_area->current_process_iterator.current_bucket != nullptr) {
        auto old_thread = *processor_area->current_thread_iterator;

        old_thread->frame = *frame;

        old_thread->is_resident = false;
    }

    // Re-enable interrupts
    asm volatile(
        "sti"
    );

    enter_next_process(processor_area, global_bitmap, &global_processes);
}

extern "C" void preempt_timer_handler(ThreadStackFrame *frame) {
    if(frame->interrupt_frame.code_segment == 0x08) {
        auto processor_area = &global_processor_areas[get_processor_id()];

        if(processor_area->in_syscall_or_user_exception) {
            processor_area->preempt_during_syscall_or_user_exception = true;

            // Send the End of Interrupt signal
            processor_area->apic_registers->end_of_interrupt.value = 0;
        } else {
            // Re-enable interrupts
            asm volatile(
                "sti"
            );

            // This path is only taken when the processor is idled in enter_next_process
            continue_in_function(frame, &preempt_timer_handler_continued);
        }
    } else {
        // Re-enable interrupts
        asm volatile(
            "sti"
        );

        continue_in_function(frame, &preempt_timer_handler_continued);
    }
}

extern "C" void spurious_interrupt_handler(const ThreadStackFrame *frame) {
    printf("Spurious interrupt at %p\n", frame->interrupt_frame.instruction_pointer);
}

volatile bool global_kernel_tables_update_lock = false;
volatile size_t global_kernel_tables_update_pages_start;
volatile size_t global_kernel_tables_update_page_count;
volatile size_t global_kernel_tables_update_progress;

void kernel_page_tables_update_handler_continued(ThreadStackFrame *frame) {
    auto processor_area = &global_processor_areas[get_processor_id()];

    // Send the End of Interrupt signal
    processor_area->apic_registers->end_of_interrupt.value = 0;
}

extern "C" void kernel_page_tables_update_handler(ThreadStackFrame *frame) {
    size_t current_pml4_table;
    asm volatile(
        "mov %%cr3, %0"
        : "=r"(current_pml4_table)
    );

    if(current_pml4_table == (size_t)&kernel_pml4_table) {
        for(
            size_t absolute_page_index = global_kernel_tables_update_pages_start;
            absolute_page_index < global_kernel_tables_update_pages_start + global_kernel_tables_update_page_count;
            absolute_page_index += 1
        ) {
            asm volatile(
                "invlpg (%0)"
                :
                : "D"(absolute_page_index * page_size)
            );
        }
    }

    atomic_add(&global_kernel_tables_update_progress, (size_t)1);

    continue_in_function_return(frame, &kernel_page_tables_update_handler_continued);
}

const size_t idt_length = 48;

#define idt_entry_exception(index) {\
    (uint16_t)(size_t)&exception_handler_thunk_##index, \
    0x08, \
    0, \
    0, \
    0xF, \
    false, \
    0, \
    true, \
    (uint64_t)(size_t)&exception_handler_thunk_##index >> 16, \
    0 \
}

#define idt_entry_general(name) {\
    (uint16_t)(size_t)&name##_handler_thunk, \
    0x08, \
    0, \
    0, \
    0xE, \
    false, \
    0, \
    true, \
    (uint64_t)(size_t)&name##_handler_thunk >> 16, \
    0 \
}

#define idt_entry_legacy_pic() {\
    (uint16_t)(size_t)&legacy_pic_dumping_ground, \
    0x08, \
    0, \
    0, \
    0xE, \
    false, \
    0, \
    true, \
    (uint64_t)(size_t)&legacy_pic_dumping_ground >> 16, \
    0 \
}

const size_t kernel_page_tables_update_vector = 33;
const size_t legacy_pic_vectors_start = 34;

IDTEntry idt_entries[idt_length] {
    idt_entry_exception(0),
    idt_entry_exception(1),
    idt_entry_exception(2),
    idt_entry_exception(3),
    idt_entry_exception(4),
    idt_entry_exception(5),
    idt_entry_exception(6),
    idt_entry_exception(7),
    idt_entry_exception(8),
    {},
    idt_entry_exception(10),
    idt_entry_exception(11),
    idt_entry_exception(12),
    idt_entry_exception(13),
    idt_entry_exception(14),
    idt_entry_exception(15),
    idt_entry_exception(16),
    idt_entry_exception(17),
    idt_entry_exception(18),
    idt_entry_exception(19),
    idt_entry_exception(20),
    {}, {}, {}, {}, {}, {}, {}, {}, {},
    idt_entry_exception(30),
    {},
    idt_entry_general(preempt_timer),
    idt_entry_general(kernel_page_tables_update),
    idt_entry_legacy_pic(),
    idt_entry_legacy_pic(),
    idt_entry_legacy_pic(),
    idt_entry_legacy_pic(),
    idt_entry_legacy_pic(),
    idt_entry_legacy_pic(),
    idt_entry_legacy_pic(),
    idt_entry_legacy_pic(),
    {}, {}, {}, {}, {},
    idt_entry_general(spurious_interrupt)
};

void send_kernel_page_tables_update(size_t pages_start, size_t page_count) {
    acquire_lock(&global_kernel_tables_update_lock);

    global_kernel_tables_update_pages_start = pages_start;
    global_kernel_tables_update_page_count = page_count;
    global_kernel_tables_update_progress = 0;

    // Make sure all memory writes are global
    asm volatile("mfence");

    auto processor_area = &global_processor_areas[get_processor_id()];

    // Set vector number, set delivery mode to fixed, set destination mode to physical,
    // set level assert, set edge trigger, set all-excluding-self shorthand
    processor_area->apic_registers->interrupt_command_lower.value = kernel_page_tables_update_vector | 1 << 14 | 0b11 << 18;

    // Wait for delivery of the IPI
    while((processor_area->apic_registers->interrupt_command_lower.value & (1 << 12)) != 0) {
        asm volatile("pause");
    }

    while(global_kernel_tables_update_progress != global_processor_count - 1) {
        asm volatile("pause");
    }

    global_kernel_tables_update_lock = false;
}

IDTDescriptor idt_descriptor { idt_length * sizeof(IDTEntry) - 1, (uint64_t)&idt_entries };

extern "C" void syscall_thunk();

#define bits_to_mask(bits) ((1 << (bits)) - 1)

inline void clear_pages(size_t page_index, size_t page_count) {
    fill_memory((void*)(page_index * page_size), page_count * page_size, 0);
}

enum struct MapProcessMemoryResult {
    Success,
    OutOfMemory,
    InvalidMemoryRange
};

static MapProcessMemoryResult map_process_memory_into_kernel(Process *process, size_t user_memory_start, size_t size, void **kernel_memory_start) {
    auto user_memory_end = user_memory_start + size;

    auto user_pages_start = user_memory_start / page_size;
    auto user_pages_end = divide_round_up(user_memory_end, page_size);
    auto page_count = user_pages_end - user_pages_start;

    auto offset = user_memory_start - user_pages_start * page_size;

    auto found = false;
    for(auto iterator = begin(process->mappings); iterator != end(process->mappings); ++iterator) {
        auto mapping = *iterator;

        if(
            user_pages_start >= mapping->logical_pages_start &&
            user_pages_end <= mapping->logical_pages_start + mapping->page_count
        ) {
            found = true;
            break;
        }
    }

    if(!found) {
        return MapProcessMemoryResult::InvalidMemoryRange;
    }

    size_t kernel_pages_start;
    if(!map_pages_from_user(
        user_pages_start,
        page_count,
        process->pml4_table_physical_address,
        global_bitmap,
        &kernel_pages_start
    )) {
        return MapProcessMemoryResult::OutOfMemory;
    }

    *kernel_memory_start = (void*)(kernel_pages_start * page_size + offset);
    return MapProcessMemoryResult::Success;
}

void syscall_entrance_continued(ThreadStackFrame *stack_frame) {
    auto processor_area = &global_processor_areas[get_processor_id()];

    processor_area->in_syscall_or_user_exception = true;

    asm volatile(
        "sti"
    );

    auto process = *processor_area->current_process_iterator;
    auto thread = *processor_area->current_thread_iterator;

    auto syscall_index = stack_frame->rbx;
    auto parameter_1 = stack_frame->rdx;
    // auto parameter_2 = stack_frame->rsi;

    auto return_1 = &stack_frame->rbx;
    auto return_2 = &stack_frame->rdx;

    static_assert(configuration_area_size == page_size, "PCI-E MMIO area not page-sized");

    switch((SyscallType)syscall_index) {
        case SyscallType::Exit: {
            destroy_process(processor_area->current_process_iterator, global_bitmap);

            enter_next_process(processor_area, global_bitmap, &global_processes);
        } break;

        case SyscallType::RelinquishTime: {
            thread->frame = *stack_frame;

            thread->is_resident = false;

            enter_next_process(processor_area, global_bitmap, &global_processes);
        } break;

        case SyscallType::DebugPrint: {
            putchar((char)parameter_1);
        } break;

        case SyscallType::MapFreeMemory: {
            *return_1 = 0;

            auto page_count = divide_round_up(parameter_1, page_size);

            size_t kernel_pages_start;
            if(!map_and_allocate_pages(page_count, global_bitmap, &kernel_pages_start)) {
                break;
            }

            size_t user_pages_start;
            if(!map_pages_from_kernel(
                kernel_pages_start,
                page_count,
                PagePermissions::Write,
                process->pml4_table_physical_address,
                global_bitmap,
                &user_pages_start
            )) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                break;
            }

            if(!register_process_mapping(process, user_pages_start, page_count, false, true, global_bitmap)) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                unmap_pages(user_pages_start, page_count, process->pml4_table_physical_address, false, global_bitmap);

                break;
            }

            clear_pages(kernel_pages_start, page_count);

            unmap_pages(kernel_pages_start, page_count);

            *return_1 = user_pages_start * page_size;
        } break;

        case SyscallType::MapFreeConsecutiveMemory: {
            *return_1 = 0;

            auto page_count = divide_round_up(parameter_1, page_size);

            size_t physical_pages_start;
            if(!allocate_consecutive_physical_pages(
                page_count,
                global_bitmap,
                &physical_pages_start
            )) {
                break;
            }

            size_t kernel_pages_start;
            if(!map_pages(physical_pages_start, page_count, global_bitmap, &kernel_pages_start)) {
                break;
            }

            size_t user_pages_start;
            if(!map_pages(
                physical_pages_start,
                page_count,
                PagePermissions::Write,
                process->pml4_table_physical_address,
                global_bitmap,
                &user_pages_start
            )) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                break;
            }

            if(!register_process_mapping(process, user_pages_start, page_count, false, true, global_bitmap)) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                unmap_pages(user_pages_start, page_count, process->pml4_table_physical_address, false, global_bitmap);

                break;
            }

            clear_pages(kernel_pages_start, page_count);

            unmap_pages(kernel_pages_start, page_count);

            *return_1 = user_pages_start * page_size;
            *return_2 = physical_pages_start * page_size;
        } break;

        case SyscallType::CreateSharedMemory: {
            *return_1 = 0;

            auto size = parameter_1;
            auto page_count = divide_round_up(size, page_size);

            size_t kernel_pages_start;
            if(!map_and_allocate_pages(page_count, global_bitmap, &kernel_pages_start)) {
                break;
            }

            size_t user_pages_start;
            if(!map_pages_from_kernel(
                kernel_pages_start,
                page_count,
                PagePermissions::Write,
                process->pml4_table_physical_address,
                global_bitmap,
                &user_pages_start
            )) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                break;
            }

            if(!register_process_mapping(process, user_pages_start, page_count, true, true, global_bitmap)) {
                unmap_and_deallocate_pages(kernel_pages_start, page_count, global_bitmap);

                unmap_pages(user_pages_start, page_count, process->pml4_table_physical_address, false, global_bitmap);

                break;
            }

            clear_pages(kernel_pages_start, page_count);

            unmap_pages(kernel_pages_start, page_count);

            *return_1 = user_pages_start * page_size;
        } break;

        case SyscallType::MapSharedMemory: {
            const MapSharedMemoryParameters *parameters;
            switch(map_process_memory_into_kernel(process, parameter_1, sizeof(MapSharedMemoryParameters), (void**)&parameters)) {
                case MapProcessMemoryResult::Success: {
                    auto target_logical_pages_start = parameters->address / page_size;
                    auto target_logical_pages_end = divide_round_up(parameters->address + parameters->size, page_size);

                    auto page_count = target_logical_pages_end - target_logical_pages_start;

                    *return_1 = (size_t)MapSharedMemoryResult::InvalidProcessID;
                    for(auto target_process : global_processes) {
                        if(target_process->id == parameters->process_id && target_process->is_ready) {
                            *return_1 = (size_t)MapSharedMemoryResult::InvalidMemoryRange;
                            for(auto mapping : target_process->mappings) {
                                if(
                                    mapping->logical_pages_start == target_logical_pages_start &&
                                    mapping->page_count == page_count &&
                                    mapping->is_shared
                                ) {
                                    size_t logical_pages_start;
                                    if(!map_pages_between_user(
                                        target_logical_pages_start,
                                        page_count,
                                        PagePermissions::Write,
                                        target_process->pml4_table_physical_address,
                                        process->pml4_table_physical_address,
                                        global_bitmap,
                                        &logical_pages_start
                                    )) {
                                        *return_1 = (size_t)MapSharedMemoryResult::OutOfMemory;
                                        break;
                                    }

                                    if(!register_process_mapping(process, logical_pages_start, page_count, true, false, global_bitmap)) {
                                        *return_1 = (size_t)MapSharedMemoryResult::OutOfMemory;

                                        break;
                                    }

                                    *return_1 = (size_t)MapSharedMemoryResult::Success;
                                    *return_2 = logical_pages_start * page_size;

                                    break;
                                }
                            }

                            break;
                        }
                    }

                    unmap_memory((void*)parameters, sizeof(MapSharedMemoryParameters));
                } break;

                case MapProcessMemoryResult::OutOfMemory: {
                    *return_1 = (size_t)MapSharedMemoryResult::OutOfMemory;
                } break;

                case MapProcessMemoryResult::InvalidMemoryRange: {
                    *return_1 = (size_t)MapSharedMemoryResult::InvalidMemoryRange;
                } break;
            }
        } break;

        case SyscallType::UnmapMemory: {
            auto logical_pages_start = parameter_1 / page_size;

            for(auto iterator = begin(process->mappings); iterator != end(process->mappings); ++iterator) {
                auto mapping = *iterator;

                if(mapping->logical_pages_start == logical_pages_start) {
                    remove_item_from_bucket_array(iterator);

                    unmap_pages(
                        mapping->logical_pages_start,
                        mapping->page_count,
                        process->pml4_table_physical_address,
                        mapping->is_owned,
                        global_bitmap
                    );

                    break;
                }
            }
        } break;

        case SyscallType::CreateProcess: {
            const CreateProcessParameters *parameters;
            switch(map_process_memory_into_kernel(process, parameter_1, sizeof(CreateProcessParameters), (void**)&parameters)) {
                case MapProcessMemoryResult::Success: {
                    uint8_t *elf_binary;
                    switch(map_process_memory_into_kernel(process, (size_t)parameters->elf_binary, parameters->elf_binary_size, (void**)&elf_binary)) {
                        case MapProcessMemoryResult::Success: {
                            void *data;
                            if(parameters->data == nullptr || parameters->data_size == 0) {
                                data = nullptr;
                            } else {
                                auto success = true;
                                switch(map_process_memory_into_kernel(process, (size_t)parameters->data, parameters->data_size, &data)) {
                                    case MapProcessMemoryResult::Success: break;

                                    case MapProcessMemoryResult::OutOfMemory: {
                                        *return_1 = (size_t)CreateProcessResult::OutOfMemory;

                                        success = true;
                                    } break;

                                    case MapProcessMemoryResult::InvalidMemoryRange: {
                                        *return_1 = (size_t)CreateProcessResult::InvalidMemoryRange;

                                        success = true;
                                    } break;
                                }

                                if(!success) {
                                    break;
                                }
                            }

                            Process *new_process;
                            Processes::Iterator new_process_iterator;
                            switch(create_process_from_elf(
                                elf_binary,
                                parameters->elf_binary_size,
                                data,
                                parameters->data_size,
                                global_bitmap,
                                global_processor_area_count,
                                global_processor_areas_physical_address,
                                &global_processes,
                                &new_process,
                                &new_process_iterator
                            )) {
                                case CreateProcessFromELFResult::Success: {
                                    *return_1 = (size_t)CreateProcessResult::Success;
                                    *return_2 = new_process->id;
                                } break;

                                case CreateProcessFromELFResult::OutOfMemory: {
                                    *return_1 = (size_t)CreateProcessResult::OutOfMemory;
                                } break;

                                case CreateProcessFromELFResult::InvalidELF: {
                                    *return_1 = (size_t)CreateProcessResult::InvalidELF;
                                } break;

                                default: halt();
                            }

                            if(parameters->data == nullptr && parameters->data_size == 0) {
                                unmap_memory(data, parameters->data_size);
                            }

                            unmap_memory(elf_binary, parameters->elf_binary_size);
                        } break;

                        case MapProcessMemoryResult::OutOfMemory: {
                            *return_1 = (size_t)CreateProcessResult::OutOfMemory;
                        } break;

                        case MapProcessMemoryResult::InvalidMemoryRange: {
                            *return_1 = (size_t)CreateProcessResult::InvalidMemoryRange;
                        } break;
                    }

                    unmap_memory((void*)parameters, sizeof(CreateProcessParameters));
                } break;

                case MapProcessMemoryResult::OutOfMemory: {
                    *return_1 = (size_t)MapSharedMemoryResult::OutOfMemory;
                } break;

                case MapProcessMemoryResult::InvalidMemoryRange: {
                    *return_1 = (size_t)MapSharedMemoryResult::InvalidMemoryRange;
                } break;
            }
        } break;

        case SyscallType::DoesProcessExist: {
            auto process_id = parameter_1;

            *return_1 = 0;

            for(auto process : global_processes) {
                if(process->id == process_id && process->is_ready) {
                    *return_1 = 1;
                    break;
                }
            }
        } break;

        case SyscallType::FindPCIEDevice: {
            const FindPCIEDeviceParameters *parameters;
            switch(map_process_memory_into_kernel(process, parameter_1, sizeof(FindPCIEDeviceParameters), (void**)&parameters)) {
                case MapProcessMemoryResult::Success: {
                    MCFGTable *mcfg_table;
                    {
                        //
                        // !!!!!!!!!!!!!!!TABLE ISN'T BEING FOUND!!!!!!!!!!!!!!!!
                        //
                        auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);
                        if(status != AE_OK) {
                            *return_1 = (size_t)FindPCIEDeviceResult::NotFound;

                            break;
                        }
                    }

                    *return_1 = (size_t)FindPCIEDeviceResult::NotFound;

                    size_t current_index = 0;

                    auto done = false;
                    for(size_t i = 0; i < (mcfg_table->preamble.Header.Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION); i += 1) {
                        auto physical_memory_start = (size_t)mcfg_table->allocations[i].Address;
                        auto segment = (size_t)mcfg_table->allocations[i].PciSegment;
                        auto start_bus_number = (size_t)mcfg_table->allocations[i].StartBusNumber;
                        auto end_bus_number = (size_t)mcfg_table->allocations[i].EndBusNumber;
                        auto bus_count = end_bus_number - start_bus_number + 1;

                        for(size_t bus = 0; bus < bus_count; bus += 1) {
                            auto bus_memory_size = device_count * function_count * configuration_area_size;

                            auto bus_memory = map_memory(
                                physical_memory_start + bus * device_count * function_count * configuration_area_size,
                                bus_memory_size,
                                global_bitmap
                            );
                            if(bus_memory == nullptr) {
                                done = true;
                                break;
                            }

                            for(size_t device = 0; device < device_count; device += 1) {
                                for(size_t function = 0; function < function_count; function += 1) {
                                    auto device_base_index = 
                                        device * function_count * configuration_area_size +
                                        function * configuration_area_size;

                                    auto header = (volatile PCIHeader*)((size_t)bus_memory + device_base_index);

                                    if(header->vendor_id == 0xFFFF) {
                                        continue;
                                    }

                                    if(!(
                                        (parameters->require_vendor_id && header->vendor_id != parameters->vendor_id) ||
                                        (parameters->require_device_id && header->device_id != parameters->device_id) ||
                                        (parameters->require_class_code && header->class_code != parameters->class_code) ||
                                        (parameters->require_subclass && header->subclass != parameters->subclass) ||
                                        (parameters->require_interface && header->interface != parameters->interface)
                                    )) {
                                        if(current_index == parameters->index) {
                                            *return_1 = (size_t)FindPCIEDeviceResult::Success;
                                            *return_2 =
                                                function |
                                                device << function_bits |
                                                bus << (function_bits + device_bits) |
                                                segment << (function_bits + device_bits + bus_bits);

                                            done = true;
                                            break;
                                        } else {
                                            current_index += 1;
                                        }
                                    }
                                }

                                if(done) {
                                    break;
                                }
                            }

                            unmap_memory((void*)bus_memory, bus_memory_size);

                            if(done) {
                                break;
                            }
                        }

                        if(done) {
                            break;
                        }
                    }

                    AcpiPutTable(&mcfg_table->preamble.Header);

                    unmap_memory((void*)parameters, sizeof(FindPCIEDeviceParameters));   
                } break;

                case MapProcessMemoryResult::OutOfMemory: {
                    *return_1 = (size_t)FindPCIEDeviceResult::OutOfMemory;
                } break;

                case MapProcessMemoryResult::InvalidMemoryRange: {
                    *return_1 = (size_t)FindPCIEDeviceResult::InvalidMemoryRange;
                } break;
            }
        } break;

        case SyscallType::MapPCIEConfiguration: {
            auto function = parameter_1 & bits_to_mask(function_bits);
            auto device = parameter_1 >> function_bits & bits_to_mask(device_bits);
            auto bus = parameter_1 >> (function_bits + device_bits) & bits_to_mask(bus_bits);
            auto target_segment = parameter_1 >> (function_bits + device_bits + bus_bits);

            MCFGTable *mcfg_table;
            {
                auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);
                if(status != AE_OK) {
                    break;
                }
            }

            *return_1 = 0;

            for(size_t i = 0; i < (mcfg_table->preamble.Header.Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION); i += 1) {
                auto physical_memory_start = (size_t)mcfg_table->allocations[i].Address;
                auto segment = (size_t)mcfg_table->allocations[i].PciSegment;

                if(segment == target_segment) {
                    size_t logical_pages_start;
                    if(!map_pages(
                        physical_memory_start / page_size +
                            bus * device_count * function_count +
                            device * function_count +
                            function,
                        1,
                        PagePermissions::Write,
                        process->pml4_table_physical_address,
                        global_bitmap,
                        &logical_pages_start
                    )) {
                        break;
                    }

                    if(!register_process_mapping(process, logical_pages_start, 1, false, false, global_bitmap)) {
                        break;
                    }

                    *return_1 = logical_pages_start * page_size;

                    break;
                }
            }

            AcpiPutTable(&mcfg_table->preamble.Header);
        } break;

        case SyscallType::MapPCIEBar: {
            auto bar_index = parameter_1 & bits_to_mask(bar_index_bits);
            auto function = parameter_1 >> bar_index_bits & bits_to_mask(function_bits);
            auto device = parameter_1 >> (bar_index_bits + function_bits) & bits_to_mask(device_bits);
            auto bus = parameter_1 >> (bar_index_bits + function_bits + device_bits) & bits_to_mask(bus_bits);
            auto target_segment = parameter_1 >> (bar_index_bits + function_bits + device_bits + bus_bits);

            MCFGTable *mcfg_table;
            {
                auto status = AcpiGetTable((char*)ACPI_SIG_MCFG, 1, (ACPI_TABLE_HEADER**)&mcfg_table);
                if(status != AE_OK) {
                    break;
                }
            }

            *return_1 = 0;

            for(size_t i = 0; i < (mcfg_table->preamble.Header.Length - sizeof(ACPI_TABLE_MCFG)) / sizeof(ACPI_MCFG_ALLOCATION); i += 1) {
                auto physical_memory_start = (size_t)mcfg_table->allocations[i].Address;
                auto segment = (size_t)mcfg_table->allocations[i].PciSegment;

                if(segment == target_segment) {
                    auto configuration_memory = map_memory(
                        physical_memory_start +
                            (bus * device_count * function_count +
                            device * function_count +
                            function) * configuration_area_size,
                        configuration_area_size,
                        global_bitmap
                    );
                    if(configuration_memory == nullptr) {
                        break;
                    }

                    auto header = (volatile PCIHeader*)configuration_memory;

                    auto bar_value = header->bars[bar_index];

                    auto bar_type = bar_value & bits_to_mask(bar_type_bits);

                    size_t address;
                    size_t size;
                    auto valid_bar = true;
                    if(bar_type == 0) { // Memory BAR
                        auto memory_bar_type = bar_value >> bar_type_bits & bits_to_mask(memory_bar_type_bits);

                        const auto info_bits = bar_type_bits + memory_bar_type_bits + memory_bar_prefetchable_bits;

                        switch(memory_bar_type) {
                            case 0b00: { // 32-bit memory BAR
                                address = (size_t)(bar_value & ~bits_to_mask(info_bits));

                                header->bars[bar_index] = -1;

                                auto temp_bar_value = header->bars[bar_index] & ~bits_to_mask(info_bits);

                                temp_bar_value = ~temp_bar_value;
                                temp_bar_value += 1;

                                size = (size_t)temp_bar_value;

                                header->bars[bar_index] = bar_value;
                            } break;

                            case 0b10: { // 64-bit memory BAR
                                auto second_bar_value = header->bars[bar_index + 1];

                                address = (size_t)(bar_value & ~bits_to_mask(info_bits)) | (size_t)second_bar_value << 32;

                                header->bars[bar_index] = -1;
                                header->bars[bar_index + 1] = -1;

                                size = (header->bars[bar_index] & ~bits_to_mask(info_bits)) | (size_t)header->bars[bar_index + 1] << 32;

                                size = ~size;
                                size += 1;

                                header->bars[bar_index] = bar_value;
                                header->bars[bar_index + 1] = second_bar_value;
                            } break;

                            default: {
                                valid_bar = false;
                            } break;
                        }
                    } else { // IO BAR
                        valid_bar = false;
                    }

                    if(!valid_bar) {
                        break;
                    }

                    unmap_memory(configuration_memory, configuration_area_size);

                    auto physical_pages_start = address / page_size;
                    auto physical_pages_end = divide_round_up(address + size, page_size);
                    auto page_count = physical_pages_end - physical_pages_start;

                    size_t logical_pages_start;
                    if(!map_pages(
                        physical_pages_start,
                        page_count,
                        PagePermissions::Write,
                        process->pml4_table_physical_address,
                        global_bitmap,
                        &logical_pages_start
                    )) {
                        break;
                    }

                    if(!register_process_mapping(process, logical_pages_start, page_count, false, false, global_bitmap)) {
                        break;
                    }

                    *return_1 = logical_pages_start * page_size;
                }
            }

            AcpiPutTable(&mcfg_table->preamble.Header);
        } break;

        default: { // unknown syscall
            printf("Unknown syscall from process %zu at %p\n", process->id, stack_frame->interrupt_frame.instruction_pointer);

            // Disable APIC timer and clear any pending interrupt

            // Disable APIC timer
            processor_area->apic_registers->lvt_timer.value |= 1 << 16;

            asm volatile(
                "sti"
            );

            processor_area->in_syscall_or_user_exception = false;
            processor_area->preempt_during_syscall_or_user_exception = false;

            destroy_process(processor_area->current_process_iterator, global_bitmap);

            enter_next_process(processor_area, global_bitmap, &global_processes);
        } break;
    }

    // Check for preempt during syscall

    asm volatile(
        "cli"
    );

    if(processor_area->preempt_during_syscall_or_user_exception) {
        asm volatile(
            "sti"
        );

        processor_area->in_syscall_or_user_exception = false;
        processor_area->preempt_during_syscall_or_user_exception = false;

        thread->frame = *stack_frame;

        thread->is_resident = false;

        enter_next_process(processor_area, global_bitmap, &global_processes);
    }

    processor_area->in_syscall_or_user_exception = false;
}

extern "C" void syscall_entrance(ThreadStackFrame *stack_frame) {
    continue_in_function_return(stack_frame, &syscall_entrance_continued);
}

static bool find_free_physical_pages_in_bootstrap(
    ConstArray<BootstrapMemoryMapEntry> bootstrap_memory_map,
    size_t page_count,
    size_t extra_pages_start,
    size_t extra_page_count,
    size_t *pages_start
) {
    for(size_t i = 0; i < bootstrap_memory_map.length; i += 1) {
        auto entry = &bootstrap_memory_map[i];
        if(entry->available) {
            auto start = entry->physical_address;
            auto end = start + entry->length;

            auto entry_pages_start = divide_round_up(start, page_size);
            auto entry_pages_end = end / page_size;
            auto entry_page_count = entry_pages_end - entry_pages_start;

            if(entry_page_count >= page_count) {
                for(size_t test_pages_start = entry_pages_start; test_pages_start < entry_pages_start - page_count; test_pages_start += 1) {
                    if(
                        !do_ranges_intersect(test_pages_start, test_pages_start + page_count, kernel_pages_start, kernel_pages_end) &&
                        !do_ranges_intersect(test_pages_start, test_pages_start + page_count, extra_pages_start, extra_pages_start + extra_page_count)
                    ) {
                        *pages_start = test_pages_start;
                        return true;
                    }
                }
            }
        }
    }

    return false;
}

inline uint32_t read_io_apic_register(volatile uint32_t *registers, size_t index) {
    registers[0] = (uint32_t)(index & 0xFF);
    return registers[4];
}

inline void write_io_apic_register(volatile uint32_t *registers, size_t index, uint32_t value) {
    registers[0] = index & 0xFF;
    registers[4] = value;
}

volatile uint32_t* global_io_apic_registers;

extern void (*init_array_start[])();
extern void (*init_array_end[])();

enum struct MSR : uint32_t {
    IA32_APIC_BASE = 0x1B,
    IA32_EFER = 0xC0000080,
    IA32_STAR = 0xC0000081,
    IA32_LSTAR = 0xC0000082,
    IA32_FMASK = 0xC0000084,
    IA32_KERNEL_GS_BASE = 0xC0000102
};

static inline void read_msr(MSR msr, uint32_t* low_part, uint32_t* high_part) {
    asm volatile(
        "rdmsr"
        : "=a"(*low_part), "=d"(*high_part)
        : "c"(msr)
    );
}

static inline uint64_t read_msr(MSR msr) {
    uint32_t low_part;
    uint32_t high_part;
    read_msr(msr, &low_part, &high_part);

    return (uint64_t)low_part | (uint64_t)high_part << 32;
}

static inline void write_msr(MSR msr, uint32_t low_part, uint32_t high_part) {
    asm volatile(
        "wrmsr"
        :
        : "a"(low_part), "d"(high_part), "c"(msr)
    );
}

static inline void write_msr(MSR msr, uint64_t value) {
    write_msr(msr, (uint32_t)value, (uint32_t)(value >> 32));
}

static ProcessorArea *setup_processor(ProcessorArea *processor_areas, MADTTable *madt_table, Array<uint8_t> bitmap) {
    auto processor_id = get_processor_id();

    auto processor_area = &processor_areas[processor_id];

    auto processor_area_user_address = user_processor_areas_memory_start + processor_id * sizeof(ProcessorArea);

    processor_area->user_address = processor_area_user_address;

    // Store user processor area pointer in GS for syscall entry to use with swapgs
    write_msr(MSR::IA32_KERNEL_GS_BASE, processor_area_user_address);

    // The processor areas are mapped into each process at the same position, so each processor stack is always in the same position

    auto stack_user_address = processor_area_user_address + offsetof(ProcessorArea, stack);

    processor_area->tss_entry.iopb_offset = (uint16_t)sizeof(TSSEntry);
    processor_area->tss_entry.rsp0 = stack_user_address + processor_stack_size;

    auto tss_entry_user_address = processor_area_user_address + offsetof(ProcessorArea, tss_entry);

    processor_area->gdt_entries[1].normal_segment.present = true;
    processor_area->gdt_entries[1].normal_segment.type = true;
    processor_area->gdt_entries[1].normal_segment.read_write = true;
    processor_area->gdt_entries[1].normal_segment.executable = true;
    processor_area->gdt_entries[1].normal_segment.long_mode = true;

    processor_area->gdt_entries[2].normal_segment.present = true;
    processor_area->gdt_entries[2].normal_segment.type = true;
    processor_area->gdt_entries[2].normal_segment.read_write = true;

    processor_area->gdt_entries[3].normal_segment.present = true;
    processor_area->gdt_entries[3].normal_segment.type = true;
    processor_area->gdt_entries[3].normal_segment.read_write = true;
    processor_area->gdt_entries[3].normal_segment.privilege = 3;

    processor_area->gdt_entries[4].normal_segment.present = true;
    processor_area->gdt_entries[4].normal_segment.type = true;
    processor_area->gdt_entries[4].normal_segment.read_write = true;
    processor_area->gdt_entries[4].normal_segment.executable = true;
    processor_area->gdt_entries[4].normal_segment.long_mode = true;
    processor_area->gdt_entries[4].normal_segment.privilege = 3;

    processor_area->gdt_entries[5].task_state_segment_low.present = true;
    processor_area->gdt_entries[5].task_state_segment_low.type = 0b01001;
    processor_area->gdt_entries[5].task_state_segment_low.base_low = (uint32_t)tss_entry_user_address;
    processor_area->gdt_entries[5].task_state_segment_low.base_mid = (uint8_t)(tss_entry_user_address >> 24);
    processor_area->gdt_entries[5].task_state_segment_low.limit_low = (uint16_t)sizeof(TSSEntry);
    processor_area->gdt_entries[5].task_state_segment_low.limit_high = (uint8_t)(sizeof(TSSEntry) >> 16);

    processor_area->gdt_entries[6].task_state_segment_high.base_high = (uint32_t)(tss_entry_user_address >> 32);

    GDTDescriptor gdt_descriptor {
        (uint16_t)(gdt_size * sizeof(GDTEntry) - 1),
        (size_t)&processor_area->gdt_entries
    };

    asm volatile(
        // Load GDT
        "lgdtq (%0)\n"

        // Load data segments
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"

        // Load code segment
        "pushq $0x08\n"
        "pushq $.long_jump_target\n"
        "lretq\n"
        ".long_jump_target:\n"

        // Load task state register
        "mov $0x2B, %%ax\n"
        "ltr %%ax"

        :
        : "r"(&gdt_descriptor)
        : "ax"
    );

    auto apic_physical_address = (size_t)madt_table->preamble.Address;

    size_t current_madt_index = 0;
    while(current_madt_index < madt_table->preamble.Header.Length - sizeof(ACPI_TABLE_MADT)) {
        auto header = (ACPI_SUBTABLE_HEADER*)((size_t)madt_table->entries + current_madt_index);

        if(header->Type == ACPI_MADT_TYPE_LOCAL_APIC_OVERRIDE) {
            auto subtable = (ACPI_MADT_LOCAL_APIC_OVERRIDE*)header;

            apic_physical_address = (size_t)subtable->Address;

            break;
        }

        current_madt_index += (size_t)header->Length;
    }

    auto apic_registers = (APICRegisters*)map_memory(apic_physical_address, 0x400, bitmap);
    if(apic_registers == nullptr) {
        printf("Error: Out of memory\n");

        halt();
    }

    // Set APIC to known state
    apic_registers->lvt_corrected_machine_check_interrupt.value |= 1 << 16;
    apic_registers->lvt_timer.value |= 1 << 16;
    apic_registers->lvt_thermal_sensor.value |= 1 << 16;
    apic_registers->lvt_performance_monitoring_counters.value |= 1 << 16;
    apic_registers->lvt_lint0.value |= 1 << 16;
    apic_registers->lvt_lint1.value |= 1 << 16;
    apic_registers->lvt_error.value |= 1 << 16;

    // Enable local APIC / set spurious interrupt vector
    apic_registers->spurious_interrupt_vector.value = 0x2F | 1 << 8;

    // Set timer interrupt vector / keep timer disabled
    apic_registers->lvt_timer.value = 0x20 | 1 << 16;

    // Set timer divider to 16
    apic_registers->timer_divide_configuration.value = 3;

    processor_area->apic_registers = apic_registers;

    // Set up syscall/sysret instructions

    {
        auto value = read_msr(MSR::IA32_EFER);
        value |= 0b1;
        write_msr(MSR::IA32_EFER, value);
    }

    write_msr(MSR::IA32_FMASK, -1, 0);

    write_msr(MSR::IA32_LSTAR, (size_t)syscall_thunk);

    // sysretq adds 16 (wtf why?) to the selector to get the code selector so 0x10 ( + 16 = 0x20) is used above...
    write_msr(MSR::IA32_STAR, 0, (uint32_t)0x10 << 16 | (uint32_t)0x08);

    return processor_area;
}

[[noreturn]] static void bootstrap_processor_entry_continued() {
    // MAKE SURE the processor area for the bootstrap processor is initalized BEFORE this point

    auto bootstrap_processor_id = get_processor_id();

    auto processor_area = &global_processor_areas[bootstrap_processor_id];

    MADTTable *madt_table;
    acpi_call(
        AcpiGetTable((char*)ACPI_SIG_MADT, 1, (ACPI_TABLE_HEADER**)&madt_table),
        "Unable to get MADT ACPI table"
    );

    size_t current_madt_index = 0;
    size_t io_apic_physical_address;
    auto io_apic_found = false;
    while(current_madt_index < madt_table->preamble.Header.Length - sizeof(ACPI_TABLE_MADT)) {
        auto header = (ACPI_SUBTABLE_HEADER*)((size_t)madt_table->entries + current_madt_index);

        if(header->Type == ACPI_MADT_TYPE_IO_APIC) {
            auto subtable = (ACPI_MADT_IO_APIC*)header;

            io_apic_found = true;

            io_apic_physical_address = (size_t)subtable->Address;

            break;
        }

        current_madt_index += (size_t)header->Length;
    }

    if(!io_apic_found) {
        printf("Error: No IO APIC found\n");

        halt();
    }

    global_io_apic_registers = (volatile uint32_t*)map_memory(io_apic_physical_address, 32, global_bitmap);
    if(global_io_apic_registers == nullptr) {
        printf("Error: Out of memory\n");

        halt();
    }

    const size_t multiprocessor_binary_load_location = 0x1000;
    static_assert(multiprocessor_binary_load_location % 0x1000 == 0, "Multiprocessor binary load location not page-aligned");
    static_assert(multiprocessor_binary_load_location < 0x100000, "Multiprocessor binary load location not within first 1M");

    auto multiprocessor_binary_size = (size_t)multiprocessor_binary_end - (size_t)multiprocessor_binary;

    copy_memory(multiprocessor_binary, (void*)multiprocessor_binary_load_location, multiprocessor_binary_size);

    asm volatile(
        "sti"
    );

    current_madt_index = 0;
    while(current_madt_index < madt_table->preamble.Header.Length - sizeof(ACPI_TABLE_MADT)) {
        auto header = (ACPI_SUBTABLE_HEADER*)((size_t)madt_table->entries + current_madt_index);

        switch(header->Type) {
            case ACPI_MADT_TYPE_LOCAL_APIC: {
                auto subtable = (ACPI_MADT_LOCAL_APIC*)header;

                if(subtable->ProcessorId != bootstrap_processor_id) {
                    asm volatile("mfence");

                    // Reset error status register
                    processor_area->apic_registers->error_status.value = 0;

                    // Set target processor APIC ID
                    processor_area->apic_registers->interrupt_command_upper.value = (uint32_t)subtable->Id << 24;

                    // Set vector number to 0, set delivery mode to INIT, set destination mode to physical,
                    // set level assert, set edge trigger, set no shorthand
                    processor_area->apic_registers->interrupt_command_lower.value = 5 << 8 | 1 << 14;

                    // Wait for delivery of the INIT IPI
                    while((processor_area->apic_registers->interrupt_command_lower.value & (1 << 12)) != 0) {
                        asm volatile("pause");
                    }

                    // TODO: Need to wait 10ms for a real physical CPU to reset

                    // Set entry page number, set delivery mode to STARTUP, set destination mode to physical,
                    // set level assert, set edge trigger, set no shorthand
                    processor_area->apic_registers->interrupt_command_lower.value = 1 | 6 << 8 | 1 << 14;

                    // Wait for delivery of the STARTUP IPI
                    while((processor_area->apic_registers->interrupt_command_lower.value & (1 << 12)) != 0) {
                        asm volatile("pause");
                    }

                    // No need to wait 200us for a real physical cpu to start executing, as we wait for the flag to be set anyway

                    // Wait for additional processor to set flag
                    while(!global_additional_processor_initialized_flag) {
                        asm volatile("pause");
                    }

                    global_additional_processor_initialized_flag = false;
                }
            } break;
        }

        current_madt_index += (size_t)header->Length;
    }

    AcpiPutTable(&madt_table->preamble.Header);

    global_all_processors_initialized = true;

    // Reload TLB
    asm volatile(
        "mov %0, %%cr3"
        : : "r"(&kernel_pml4_table)
    );

    printf("Loading init process...\n");

    auto embedded_init_binary_size = (size_t)embedded_init_binary_end - (size_t)embedded_init_binary;

    Process *init_process;
    Processes::Iterator init_process_iterator;
    switch(create_process_from_elf(
        embedded_init_binary,
        embedded_init_binary_size,
        nullptr,
        0,
        global_bitmap,
        global_processor_area_count,
        global_processor_areas_physical_address,
        &global_processes,
        &init_process,
        &init_process_iterator
    )) {
        case CreateProcessFromELFResult::Success: break;

        case CreateProcessFromELFResult::OutOfMemory: {
            printf("Error: Out of memory\n");

            halt();
        } break;

        case CreateProcessFromELFResult::InvalidELF: {
            printf("Error: Init process ELF file is invalid\n");

            halt();
        } break;

        default: halt();
    }

    printf("Entering init process\n");

    enter_next_process(processor_area, global_bitmap, &global_processes);
}

[[noreturn]] static void bootstrap_processor_entry() {
    auto bootstrap_space = (BootstrapSpace*)bootstrap_space_address;

    ConstArray<BootstrapMemoryMapEntry> bootstrap_memory_map {
        bootstrap_space->memory_map,
        bootstrap_space->memory_map_size
    };

    global_acpi_table_physical_address = bootstrap_space->acpi_table_physical_address;

    // Enable sse
    asm volatile(
        "mov %%cr0, %%rax\n"
        "or $(1 << 1), %%rax\n"
        "and $~(1 << 2), %%rax\n"
        "mov %%rax, %%cr0\n"
        "mov %%cr4, %%rax\n"
        "or $(1 << 9), %%rax\n"
        "or $(1 << 10), %%rax\n"
        "mov %%rax, %%cr4"
        :
        :
        : "rax"
    );

    auto initializer_count = ((size_t)init_array_end - (size_t)init_array_start) / sizeof(void (*)());

    for(size_t i = 0; i < initializer_count; i += 1) {
        init_array_start[i]();
    }

    setup_console();

    // Perform legacy PIC initialization

    io_out(0xA0, 1 << 4 | 1 << 0);
    io_out(0x20, 1 << 4 | 1 << 0);

    io_out(0xA1, legacy_pic_vectors_start);
    io_out(0x21, legacy_pic_vectors_start);

    io_out(0xA1, 1 << 2);
    io_out(0x21, 1 << 1);

    io_out(0xA1, 1 << 0);
    io_out(0x21, 1 << 0);

    io_out(0xA1, 0xFF);
    io_out(0x21, 0xFF);

    // Load IDT
    asm volatile(
        "lidt (%0)"
        :
        : "D"(&idt_descriptor)
        : "al"
    );

    size_t next_pdp_table_index = 0;
    size_t next_pd_table_index = 0;
    size_t next_page_table_index = 0;

    for(size_t total_page_index = kernel_pages_start; total_page_index < kernel_pages_end; total_page_index += 1) {
        auto page_index = total_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        PageTableEntry *pdp_table;
        if(kernel_pml4_table[pml4_index].present) {
            pdp_table = (PageTableEntry*)(kernel_pml4_table[pml4_index].page_address * page_size);
        } else {
            pdp_table = &kernel_pdp_tables[next_pdp_table_index][0];
            next_pdp_table_index += 1;

            kernel_pml4_table[pml4_index].present = true;
            kernel_pml4_table[pml4_index].write_allowed = true;
            kernel_pml4_table[pml4_index].user_mode_allowed = true;
            kernel_pml4_table[pml4_index].page_address = (size_t)pdp_table / page_size;

            next_pdp_table_index += 1;
        }

        PageTableEntry *pd_table;
        if(pdp_table[pdp_index].present) {
            pd_table = (PageTableEntry*)(pdp_table[pdp_index].page_address * page_size);
        } else {
            pd_table = &kernel_pd_tables[next_pd_table_index][0];
            next_pd_table_index += 1;

            pdp_table[pdp_index].present = true;
            pdp_table[pdp_index].write_allowed = true;
            pdp_table[pdp_index].user_mode_allowed = true;
            pdp_table[pdp_index].page_address = (size_t)pd_table / page_size;
        }

        PageTableEntry *page_table;
        if(pd_table[pd_index].present) {
            page_table = (PageTableEntry*)(pd_table[pd_index].page_address * page_size);
        } else {
            page_table = &kernel_page_tables[next_page_table_index][0];
            next_page_table_index += 1;

            pd_table[pd_index].present = true;
            pd_table[pd_index].write_allowed = true;
            pd_table[pd_index].user_mode_allowed = true;
            pd_table[pd_index].page_address = (size_t)page_table / page_size;
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = total_page_index;
    }

    kernel_pml4_table[page_table_length - 1].present = true;
    kernel_pml4_table[page_table_length - 1].write_allowed = true;
    kernel_pml4_table[page_table_length - 1].page_address = (size_t)kernel_pml4_table / page_size;

    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_pml4_table)
    );

    size_t highest_available_memory_end = 0;
    for(size_t i = 0; i < bootstrap_memory_map.length; i += 1) {
        auto entry = &bootstrap_memory_map[i];

        if(entry->available) {
            auto end = entry->physical_address + entry->length;

            if(end > highest_available_memory_end) {        
                highest_available_memory_end = end;
            }
        }
    }

    auto highest_available_pages_end = divide_round_up(highest_available_memory_end, page_size);

    auto bitmap_size = highest_available_pages_end / 8;
    auto bitmap_page_count = divide_round_up(bitmap_size, page_size);

    size_t bitmap_physical_pages_start;
    if(!find_free_physical_pages_in_bootstrap(
        bootstrap_memory_map,
        bitmap_page_count,
        0,
        0,
        &bitmap_physical_pages_start
    )) {
        printf("Error: Out of bootstrap memory\n");

        halt();
    }

    auto new_page_table_count = count_page_tables_needed_for_logical_pages(kernel_pages_end, bitmap_page_count);

    size_t new_physical_pages_start;
    if(!find_free_physical_pages_in_bootstrap(
        bootstrap_memory_map,
        new_page_table_count,
        bitmap_physical_pages_start,
        bitmap_page_count,
        &new_physical_pages_start
    )) {
        printf("Error: Out of bootstrap memory\n");

        halt();
    }

    size_t next_new_physical_page_index = 0;

    for(size_t relative_page_index = 0; relative_page_index < bitmap_page_count; relative_page_index += 1) {
        auto page_index = kernel_pages_end + relative_page_index;
        auto pd_index = page_index / page_table_length;
        auto pdp_index = pd_index / page_table_length;
        auto pml4_index = pdp_index / page_table_length;

        page_index %= page_table_length;
        pd_index %= page_table_length;
        pdp_index %= page_table_length;
        pml4_index %= page_table_length;

        auto pml4_table = get_pml4_table_pointer();

        auto pdp_table = get_pdp_table_pointer(pml4_index);

        if(!pml4_table[pml4_index].present) {
            auto physical_pages_start = new_physical_pages_start + next_new_physical_page_index;
            next_new_physical_page_index += 1;

            pml4_table[pml4_index].present = true;
            pml4_table[pml4_index].write_allowed = true;
            pml4_table[pml4_index].user_mode_allowed = true;
            pml4_table[pml4_index].page_address = physical_pages_start;

            asm volatile(
                "invlpg (%0)"
                :
                : "D"(pdp_table)
            );

            fill_memory(pdp_table, sizeof(PageTableEntry[page_table_length]), 0);
        }

        auto pd_table = get_pd_table_pointer(pml4_index, pdp_index);

        if(!pdp_table[pdp_index].present) {
            auto physical_pages_start = new_physical_pages_start + next_new_physical_page_index;
            next_new_physical_page_index += 1;

            pdp_table[pdp_index].present = true;
            pdp_table[pdp_index].write_allowed = true;
            pdp_table[pdp_index].user_mode_allowed = true;
            pdp_table[pdp_index].page_address = physical_pages_start;

            asm volatile(
                "invlpg (%0)"
                :
                : "D"(pd_table)
            );

            fill_memory(pd_table, sizeof(PageTableEntry[page_table_length]), 0);
        }

        auto page_table = get_page_table_pointer(pml4_index, pdp_index, pd_index);

        if(!pd_table[pd_index].present) {
            auto physical_pages_start = new_physical_pages_start + next_new_physical_page_index;
            next_new_physical_page_index += 1;

            pd_table[pd_index].present = true;
            pd_table[pd_index].write_allowed = true;
            pd_table[pd_index].user_mode_allowed = true;
            pd_table[pd_index].page_address = physical_pages_start;

            asm volatile(
                "invlpg (%0)"
                :
                : "D"(page_table)
            );

            fill_memory(page_table, sizeof(PageTableEntry[page_table_length]), 0);
        }

        page_table[page_index].present = true;
        page_table[page_index].write_allowed = true;
        page_table[page_index].page_address = bitmap_physical_pages_start + relative_page_index;

        asm volatile(
            "invlpg (%0)"
            :
            : "D"((kernel_pages_end + relative_page_index) * page_size)
        );
    }

    { // Enable execution-disable page bit
        auto value = read_msr(MSR::IA32_EFER);
        value |= 1 << 11;
        write_msr(MSR::IA32_EFER, value);
    }

    Array<uint8_t> bitmap {
        (uint8_t*)(kernel_pages_end * page_size),
        bitmap_page_count * page_size
    };

    fill_memory(bitmap.data, bitmap_size, 0xFF);

    for(size_t i = 0; i < bootstrap_memory_map.length; i += 1) {
        auto entry = &bootstrap_memory_map[i];

        if(entry->available) {
            auto start = entry->physical_address;
            auto end = start + entry->length;

            auto entry_pages_start = divide_round_up(start, page_size);
            auto entry_pages_end = end / page_size;
            auto entry_page_count = entry_pages_end - entry_pages_start;

            deallocate_bitmap_range(bitmap, entry_pages_start, entry_page_count);
        }
    }

    allocate_bitmap_range(bitmap, kernel_pages_start, kernel_pages_end - kernel_pages_start);

    allocate_bitmap_range(bitmap, bitmap_physical_pages_start, bitmap_page_count);

    allocate_bitmap_range(bitmap, new_physical_pages_start, new_page_table_count);

    global_bitmap = bitmap;

    acpi_call(AcpiInitializeSubsystem(), "Unable to initialize ACPICA subsystem");

    acpi_call(AcpiInitializeTables(nullptr, 8, TRUE), "Unable to load ACPI tables");

    MADTTable *madt_table;
    acpi_call(
        AcpiGetTable((char*)ACPI_SIG_MADT, 1, (ACPI_TABLE_HEADER**)&madt_table),
        "Unable to get MADT ACPI table"
    );

    auto bootstrap_processor_id = get_processor_id();

    size_t processor_count = 1;

    auto maximum_processor_id = bootstrap_processor_id;

    size_t current_madt_index = 0;
    while(current_madt_index < madt_table->preamble.Header.Length - sizeof(ACPI_TABLE_MADT)) {
        auto header = (ACPI_SUBTABLE_HEADER*)((size_t)madt_table->entries + current_madt_index);

        switch(header->Type) {
            case ACPI_MADT_TYPE_LOCAL_APIC: {
                auto subtable = (ACPI_MADT_LOCAL_APIC*)header;

                if(subtable->ProcessorId != bootstrap_processor_id) {
                    processor_count += 1;

                    if(subtable->ProcessorId > maximum_processor_id) {
                        maximum_processor_id = subtable->ProcessorId;
                    }
                }
            } break;
        }

        current_madt_index += (size_t)header->Length;
    }

    auto processor_area_count = maximum_processor_id + 1;

    size_t processor_areas_physical_address;
    auto processor_areas = (ProcessorArea*)map_and_allocate_consecutive_memory(
        processor_area_count * sizeof(ProcessorArea),
        global_bitmap,
        &processor_areas_physical_address,
        false
    );

    fill_memory(processor_areas, processor_area_count * sizeof(ProcessorArea), 0);

    global_processor_count = processor_count;
    global_processor_area_count = processor_area_count;
    global_processor_areas_physical_address = processor_areas_physical_address;
    global_processor_areas = processor_areas;

    auto processor_area = setup_processor(global_processor_areas, madt_table, global_bitmap);

    AcpiPutTable(&madt_table->preamble.Header);

    // Change to new stack
    asm volatile(
        "mov %0, %%rsp\n"
        "call *%1"
        :
        : "r"((size_t)&processor_area->stack + processor_stack_size), "r"(bootstrap_processor_entry_continued)
    );

    unreachable();
}

[[noreturn]] static void additional_processor_entry_continued() {
    // MAKE SURE the processor area for this additional processor is initalized BEFORE this point

    global_additional_processor_initialized_flag = true;

    asm volatile(
        "sti"
    );

    auto processor_area = &global_processor_areas[get_processor_id()];

    enter_next_process(processor_area, global_bitmap, &global_processes);
}

[[noreturn]] static void additional_processor_entry() {
    // Enable sse
    asm volatile(
        "mov %%cr0, %%rax\n"
        "or $(1 << 1), %%rax\n"
        "and $~(1 << 2), %%rax\n"
        "mov %%rax, %%cr0\n"
        "mov %%cr4, %%rax\n"
        "or $(1 << 9), %%rax\n"
        "or $(1 << 10), %%rax\n"
        "mov %%rax, %%cr4"
        :
        :
        : "rax"
    );

    asm volatile(
        // Load IDT
        "lidt (%0)\n"
        // Disable PIC
        "mov $0xff, %%al\n"
        "out %%al, $0xA1\n"
        "out %%al, $0x21\n"
        :
        : "D"(&idt_descriptor)
        : "al"
    );

    { // Enable execution-disable page bit
        auto value = read_msr(MSR::IA32_EFER);
        value |= 1 << 11;
        write_msr(MSR::IA32_EFER, value);
    }

    asm volatile(
        "mov %0, %%cr3"
        :
        : "D"(&kernel_pml4_table)
    );

    MADTTable *madt_table;
    acpi_call(
        AcpiGetTable((char*)ACPI_SIG_MADT, 1, (ACPI_TABLE_HEADER**)&madt_table),
        "Unable to get MADT ACPI table"
    );

    auto processor_area = setup_processor(global_processor_areas, madt_table, global_bitmap);

    AcpiPutTable(&madt_table->preamble.Header);

    // Change to new stack
    asm volatile(
        "mov %0, %%rsp\n"
        "call *%1"
        :
        : "r"((size_t)&processor_area->stack + processor_stack_size), "r"(additional_processor_entry_continued)
    );

    unreachable();
}

// Purely here to enforce the type of main as KernelMain
extern "C" KernelMain main [[noreturn]];

extern "C" [[noreturn]] __attribute__((section(".text.main"))) void main(bool is_first_entry) {
    if(is_first_entry) {
        bootstrap_processor_entry();
    } else {
        additional_processor_entry();
    }
}

void *allocate(size_t size) {
    auto base_pointer = map_and_allocate_memory(page_size + size, global_bitmap);

    if(base_pointer == nullptr) {
        return nullptr;
    }

    if(global_all_processors_initialized) {
        send_kernel_page_tables_update_memory(base_pointer, page_size + size);
    }

    *(size_t*)base_pointer = size;

    return (void*)((size_t)base_pointer + page_size);
}

void deallocate(void *pointer) {
    auto base_pointer = (void*)((size_t)pointer - page_size);

    auto size = *(size_t*)base_pointer;

    unmap_and_deallocate_memory(base_pointer, page_size + size, global_bitmap);
}