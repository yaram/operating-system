#pragma once

#include "bucket_array.h"

template <typename T, size_t N>
static T *allocate_from_bucket_array(
    BucketArray<T, N> *bucket_array,
    BucketArrayIterator<T, N> *result_iterator = nullptr
) {
    auto iterator = find_available_bucket_slot(bucket_array);

    if(iterator.current_bucket == nullptr) {
        auto new_bucket = (Bucket<T, N>*)syscall(SyscallType::MapFreeMemory, sizeof(Bucket<T, N>), 0);
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
        **iterator = {};
    }

    iterator.current_bucket->unavailable[iterator.current_sub_index] = true;
    iterator.current_bucket->occupied[iterator.current_sub_index] = true;

    if(result_iterator != nullptr) {
        *result_iterator = iterator;
    }

    return *iterator;
}

template <typename T, size_t N>
static void deallocate_bucket(const Bucket<T, N> *bucket) {
    if(bucket->next != nullptr) {
        deallocate_bucket(bucket->next);
    }

    syscall(SyscallType::UnmapMemory, (size_t)bucket, 0);
}

template <typename T, size_t N>
inline void deallocate_bucket_array(const BucketArray<T, N> *bucket_array) {
    if(bucket_array->first_bucket.next != nullptr) {
        deallocate_bucket_array(bucket_array->first_bucket.next);
    }
}