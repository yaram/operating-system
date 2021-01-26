#pragma once

#include <stddef.h>

const size_t page_size = 4096;

void create_initial_pages(size_t identity_pages_start, size_t identity_page_count);
void map_consecutive_pages(size_t physical_pages_start, size_t logical_pages_start, size_t page_count, bool invalidate_pages);
size_t map_pages(size_t physical_page_index, size_t page_count);
void unmap_pages(size_t logical_page_index, size_t page_count);