#pragma once

#include <stddef.h>

template <typename T, size_t N>
struct Bucket {
    T entries[N];

    bool unavailable[N];
    bool occupied[N];

    Bucket<T, N> *next;
};

template <typename T, size_t N>
struct BucketArrayIterator {
    Bucket<T, N> *current_bucket;

    size_t current_sub_index;
};

template <typename T, size_t N>
static T *operator *(const BucketArrayIterator<T, N> &iterator) {
    return &iterator.current_bucket->entries[iterator.current_sub_index];
}

template <typename T, size_t N>
static void operator ++(BucketArrayIterator<T, N> &iterator) {
    if(iterator.current_bucket == nullptr) {
        return;
    }

    while(true) {
        if(iterator.current_sub_index == N - 1) {
            iterator.current_bucket = iterator.current_bucket->next;
            iterator.current_sub_index = 0;

            if(iterator.current_bucket == nullptr) {
                break;
            }
        } else {
            iterator.current_sub_index += 1;
        }

        if(iterator.current_bucket->occupied[iterator.current_sub_index]) {
            break;
        }
    }
}

template <typename T, size_t N>
static bool operator !=(const BucketArrayIterator<T, N> &a, const BucketArrayIterator<T, N> &b) {
    return a.current_bucket != b.current_bucket || a.current_sub_index != b.current_sub_index;
}

template <typename T, size_t N>
inline void remove_item_from_bucket_array(BucketArrayIterator<T, N> iterator) {
    iterator.current_bucket->occupied[iterator.current_sub_index] = false;
    iterator.current_bucket->unavailable[iterator.current_sub_index] = false;
}

template <typename T, size_t N>
struct ConstBucketArrayIterator {
    const Bucket<T, N> *current_bucket;

    size_t current_sub_index;
};

template <typename T, size_t N>
static const T *operator *(const ConstBucketArrayIterator<T, N> &iterator) {
    return &iterator.current_bucket->entries[iterator.current_sub_index];
}

template <typename T, size_t N>
static void operator ++(ConstBucketArrayIterator<T, N> &iterator) {
    if(iterator.current_bucket == nullptr) {
        return;
    }

    while(true) {
        if(iterator.current_sub_index == N - 1) {
            iterator.current_bucket = iterator.current_bucket->next;
            iterator.current_sub_index = 0;

            if(iterator.current_bucket == nullptr) {
                break;
            }
        } else {
            iterator.current_sub_index += 1;
        }

        if(iterator.current_bucket->occupied[iterator.current_sub_index]) {
            break;
        }
    }
}

template <typename T, size_t N>
static bool operator !=(const ConstBucketArrayIterator<T, N> &a, const ConstBucketArrayIterator<T, N> &b) {
    return a.current_bucket != b.current_bucket || a.current_sub_index != b.current_sub_index;
}

template <typename T, size_t N>
struct BucketArray {
    using Type = T;
    const static auto size = N;

    using Bucket = Bucket<T, N>;

    using Iterator = BucketArrayIterator<T, N>;
    using ConstIterator = ConstBucketArrayIterator<T, N>;

    Bucket first_bucket;
};

template <typename T, size_t N>
static BucketArrayIterator<T, N> find_available_bucket_slot(BucketArray<T, N> *bucket_array) {
    BucketArrayIterator<T, N> iterator {
        &bucket_array->first_bucket,
        0
    };

    while(true) {
        if(!iterator.current_bucket->unavailable[iterator.current_sub_index]) {
            break;
        }

        if(iterator.current_sub_index == N - 1) {
            iterator.current_bucket = iterator.current_bucket->next;
            iterator.current_sub_index = 0;

            if(iterator.current_bucket == nullptr) {
                break;
            }
        } else {
            iterator.current_sub_index += 1;
        }
    }

    return iterator;
}

template <typename T, size_t N>
static T *index_bucket_array(BucketArray<T, N> *bucket_array, size_t index, BucketArrayIterator<T, N> *result_iterator = nullptr) {
    BucketArrayIterator<T, N> iterator {
        &bucket_array->first_bucket,
        0
    };

    size_t current_index = 0;
    while(current_index != index) {
        if(iterator.current_bucket == nullptr) {
            break;
        }

        if(iterator.current_sub_index == N - 1) {
            iterator.current_bucket = iterator.current_bucket->next;
            iterator.current_sub_index = 0;

            if(iterator.current_bucket == nullptr) {
                break;
            }
        } else {
            iterator.current_sub_index += 1;
        }

        current_index += 1;
    }

    if(result_iterator != nullptr) {
        *result_iterator = iterator;
    }

    if(iterator.current_bucket == nullptr) {
        return nullptr;
    } else {
        return *iterator;
    }
}

template <typename T, size_t N>
static BucketArrayIterator<T, N> begin(BucketArray<T, N> &bucket_array) {
    BucketArrayIterator<T, N> iterator {
        &bucket_array.first_bucket,
        0
    };

    while(true) {
        if(iterator.current_bucket->occupied[iterator.current_sub_index]) {
            break;
        }

        if(iterator.current_sub_index == N - 1) {
            iterator.current_bucket = iterator.current_bucket->next;
            iterator.current_sub_index = 0;

            if(iterator.current_bucket == nullptr) {
                break;
            }
        } else {
            iterator.current_sub_index += 1;
        }
    }

    return iterator;
}

template <typename T, size_t N>
static BucketArrayIterator<T, N> end(BucketArray<T, N> &bucket_array) {
    return {
        nullptr,
        0
    };
}

template <typename T, size_t N>
static T *index_bucket_array(const BucketArray<T, N> *bucket_array, size_t index, ConstBucketArrayIterator<T, N> *result_iterator = nullptr) {
    ConstBucketArrayIterator<T, N> iterator {
        &bucket_array->first_bucket,
        0
    };

    size_t current_index = 0;
    while(current_index != index) {
        if(iterator.current_bucket == nullptr) {
            break;
        }

        if(iterator.current_sub_index == N - 1) {
            iterator.current_bucket = iterator.current_bucket->next;
            iterator.current_sub_index = 0;

            if(iterator.current_bucket == nullptr) {
                break;
            }
        } else {
            iterator.current_sub_index += 1;
        }

        current_index += 1;
    }

    if(result_iterator != nullptr) {
        *result_iterator = iterator;
    }

    if(iterator.current_bucket == nullptr) {
        return nullptr;
    } else {
        return *iterator;
    }
}

template <typename T, size_t N>
static ConstBucketArrayIterator<T, N> begin(const BucketArray<T, N> &bucket_array) {
    ConstBucketArrayIterator<T, N> iterator {
        &bucket_array.first_bucket,
        0
    };

    while(true) {
        if(iterator.current_bucket->occupied[iterator.current_sub_index]) {
            break;
        }

        if(iterator.current_sub_index == N - 1) {
            iterator.current_bucket = iterator.current_bucket->next;
            iterator.current_sub_index = 0;

            if(iterator.current_bucket == nullptr) {
                break;
            }
        } else {
            iterator.current_sub_index += 1;
        }
    }

    return iterator;
}

template <typename T, size_t N>
static ConstBucketArrayIterator<T, N> end(const BucketArray<T, N> &bucket_array) {
    return {
        nullptr,
        0
    };
}