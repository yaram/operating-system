#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "acpi.h"
}
#include "console.h"
#include "heap.h"
#include "paging.h"

size_t next_heap_index = 0;
uint8_t heap[1024 * 1024];

extern "C" void main() {
    clear_console();

    const size_t inital_pages_start = 0;
    const size_t inital_pages_length = 0x800000;

    create_initial_pages(inital_pages_start / page_size, inital_pages_length / page_size);
}

void *allocate(size_t size) {
    auto index = next_heap_index;

    next_heap_index += size;

    return (void*)(index + (size_t)heap);
}

void deallocate(void *pointer) {

}