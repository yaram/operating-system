#pragma once

#include "bucket_array.h"
#include "paging.h"
#include "memory.h"
#include "threading_kernel.h"
#include "multiprocessing.h"

template <typename T, size_t N>
static T *allocate_from_bucket_array(
    BucketArray<T, N> *bucket_array,
    Array<uint8_t> bitmap,
    bool is_global,
    BucketArrayIterator<T, N> *result_iterator = nullptr
) {
    while(true) {
        auto iterator = find_available_bucket_slot(bucket_array);

        if(iterator.current_bucket == nullptr) {
            auto new_bucket = (Bucket<T, N>*)map_and_allocate_memory(sizeof(Bucket<T, N>), bitmap);
            if(new_bucket == nullptr) {
                return nullptr;
            }

            memset(new_bucket, 0, sizeof(Bucket<T, N>)); // Need to memset instead of assigning to Bucket<T, N>{} for stack size reasons

            if(is_global) {
                send_kernel_page_tables_update_memory(new_bucket, sizeof(Bucket<T, N>));
            }

            while(true) {
                auto current_bucket = &bucket_array->first_bucket;
                while(current_bucket->next != nullptr) {
                    current_bucket = current_bucket->next;
                }

                if(compare_and_swap(&current_bucket->next, (Bucket<T, N>*)nullptr, new_bucket)) {
                    break;
                }
            }

            iterator = {
                new_bucket,
                0
            };
        }

        if(!compare_and_swap(&iterator.current_bucket->unavailable[iterator.current_sub_index], false, true)) {
            continue;
        }

        **iterator = {};

        iterator.current_bucket->occupied[iterator.current_sub_index] = true;

        if(result_iterator != nullptr) {
            *result_iterator = iterator;
        }

        return *iterator;
    }
}

template <typename T, size_t N>
static void unmap_and_deallocate_bucket(const Bucket<T, N> *bucket, Array<uint8_t> bitmap) {
    if(bucket->next != nullptr) {
        unmap_and_deallocate_bucket(bucket->next, bitmap);
    }

    unmap_and_deallocate_memory((void*)bucket, sizeof(Bucket<T, N>), bitmap);
}

template <typename T, size_t N>
inline void unmap_and_deallocate_bucket_array(const BucketArray<T, N> *bucket_array, Array<uint8_t> bitmap) {
    if(bucket_array->first_bucket.next != nullptr) {
        unmap_and_deallocate_bucket(bucket_array->first_bucket.next, bitmap);
    }
}