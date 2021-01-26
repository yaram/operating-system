#include <stddef.h>
#include <stdint.h>
#include "console.h"
#include "paging.h"

extern "C" void main() {
    clear_console();

    const size_t inital_pages_start = 0;
    const size_t inital_pages_length = 0x800000;

    create_initial_pages(inital_pages_start / page_size, inital_pages_length / page_size);
}