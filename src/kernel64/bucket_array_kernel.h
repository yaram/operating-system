#pragma once

#include "bucket_array.h"
#include "paging.h"
#include "memory.h"

template <typename T, size_t N>
static T *allocate_from_bucket_array(
    BucketArray<T, N> *bucket_array,
    Array<uint8_t> bitmap,
    BucketArrayIterator<T, N> *result_iterator = nullptr
) {
    auto iterator = find_unoccupied_bucket_slot(bucket_array);

    if(iterator.current_bucket == nullptr) {
        auto new_bucket = (Bucket<T, N>*)map_and_allocate_memory(sizeof(Bucket<T, N>), bitmap);
        if(new_bucket == nullptr) {
            return nullptr;
        }

        {
            auto current_bucket = &bucket_array->first_bucket;
            while(current_bucket->next != nullptr) {
                current_bucket = current_bucket->next;
            }

            current_bucket->next = new_bucket;
        }

        iterator = {
            new_bucket,
            0
        };
    } else {
        memset(*iterator, 0, sizeof(T));
    }

    iterator.current_bucket->occupied[iterator.current_sub_index] = true;

    if(result_iterator != nullptr) {
        *result_iterator = iterator;
    }

    return *iterator;
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