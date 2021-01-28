#include <stddef.h>
#include <stdint.h>
extern "C" {
#include "acpi.h"
}
#include "console.h"
#include "heap.h"
#include "paging.h"

size_t next_heap_index = 0;
const size_t heap_size = 1024 * 1024;
uint8_t heap[heap_size];

extern "C" void main() {
    clear_console();

    const size_t inital_pages_start = 0;
    const size_t inital_pages_length = 0x800000;

    if(!create_initial_pages(inital_pages_start / page_size, inital_pages_length / page_size)) {
        printf("Error: Unable to allocate initial pages\n");

        return;
    }
}

void *allocate(size_t size) {
    auto index = next_heap_index;

    next_heap_index += size;

    if(next_heap_index > heap_size) {
        return nullptr;
    }

    return (void*)(index + (size_t)heap);
}

void deallocate(void *pointer) {

}