#pragma once

#include <stddef.h>

const size_t page_size = 4096;

bool create_initial_pages(size_t identity_pages_start, size_t identity_page_count);
bool map_consecutive_pages(size_t physical_pages_start, size_t logical_pages_start, size_t page_count, bool invalidate_pages);
bool map_pages(size_t physical_pages_start, size_t page_count, size_t *logical_pages_start);
void unmap_pages(size_t logical_pages_start, size_t page_count);
void *map_memory(size_t physical_memory_start, size_t size);
void unmap_memory(void *logical_memory_start, size_t size);