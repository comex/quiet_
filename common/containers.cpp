#define WANT_UHASH 1
#define WANT_UTRIE 1
#define WANT_CALLBACK 1
#include "containers.h"
#include "misc.h"
#include "decls.h"
#include "logging.h"

BEGIN_LOCAL_DECLS

#define HEAP_IS_MALLOC (0)

#if CONTAINERTEST && 0
#undef HEAP_IS_MALLOC
#define HEAP_IS_MALLOC 0
#endif

UNUSED static inline size_t
log2_round_up(size_t x) {
    if (DUMMY && x == 0)
        return 0;
    size_t ret = 8 * sizeof(size_t) - (size_t)__builtin_clzl(x) - 1;
    return ret + (x != (size_t)1 << ret);
}

#if HEAP_IS_MALLOC
void
splitter_heap_init(struct splitter_heap *heap) {
    memset(heap, 0xdd, sizeof(*heap));
}
void
club_heap_init(struct club_heap *heap, struct splitter_heap *backing, size_t alloc_size, size_t est_group_count, size_t desired_free_count) {
    memset(heap, 0xdd, sizeof(*heap));
}
maybe<void *>
heap_alloc(struct heap *heap, size_t size) {
    return maybe(MEMAllocFromDefaultHeapEx(size, HEAP_ALLOC_ALIGN));
}
void
heap_free(struct heap *heap, void *ptr) {
    if (ptr)
        MEMFreeToDefaultHeap(ptr);
}
void heap_debug(struct heap *heap) {}

#else // HEAP_IS_MALLOC

static inline struct heap_bucket *
splitter_heap_bucket_prev(struct heap_bucket *bucket) {
    size_t prev_shift = bucket->prev_free_list_idx + HEAP_MIN_SHIFT;
    return (struct heap_bucket *)((char *)bucket - ((size_t)1 << prev_shift));
}
static inline struct heap_bucket *
splitter_heap_bucket_next(struct heap_bucket *bucket) {
    size_t shift = bucket->free_list_idx + HEAP_MIN_SHIFT;
    return (struct heap_bucket *)((char *)bucket + ((size_t)1 << shift));
}

#if HEAP_DEBUG
static void
heap_debug_bucket(struct heap_bucket *bucket) {
    log("  bucket %p: magic=%02x free_list_idx=%u prev_free_list_idx=%u in_use=%u has_next=%u pad=%u "
        "data=%p\n",
        bucket, bucket->magic, bucket->free_list_idx, bucket->prev_free_list_idx, bucket->in_use,
        bucket->has_next, bucket->pad, bucket->data);
    if (!bucket->in_use) {
        log("    prev=%p next=%p\n", bucket->free_links.prev, bucket->free_links.next);
    }
}
void
heap_debug(struct heap *heap) {
    size_t free_list_count;
    if (heap->magic == HEAP_SPLITTER) {
        log("all chunks list:\n");
        struct splitter_heap *sheap = (struct splitter_heap *)heap;
        for (struct heap_bucket *bucket : *sheap->debug_chunks_list) {
            log("chunk %p:\n", bucket);
            while (1) {
                heap_debug_bucket(bucket);
                if (!bucket->has_next)
                    break;
                bucket = splitter_heap_bucket_next(bucket);
            }
        }
        free_list_count = HEAP_MAX_SHIFT - HEAP_MIN_SHIFT;
    } else {
        ensure(heap->magic == HEAP_CLUB);
        struct club_heap *cheap = (struct club_heap *)heap;
        log("all groups list:\n");
        for (struct club_heap_group_header *header : *cheap->debug_groups_list) {
            log("group %p: magic=%x free_count=%x\n", header, header->magic, header->group_free_count);
            ensure(header->magic == CLUB_HEAP_GROUP);
            struct heap_bucket *first_bucket = (struct heap_bucket *)(header + 1);
            uint8_t free_list_idx = first_bucket->free_list_idx;
            size_t group_count = cheap->size_info[free_list_idx].group_count;
            size_t bucket_size = cheap->size_info[free_list_idx].bucket_size;
            for (size_t i = 0; i < group_count; i++) {
                struct heap_bucket *bucket = (struct heap_bucket *)((char *)(header + 1) + i * bucket_size);
                heap_debug_bucket(bucket);
                ensure(bucket->magic == HEAP_BUCKET_CLUB);
                ensure(bucket->free_list_idx == free_list_idx);
                ensure(bucket->idx_in_group == i);
            }
        }
        log("size info:\n");
        for (size_t i = 0; i < CLUB_HEAP_MAX_SIZES; i++) {
            struct club_heap::size_info *info = &cheap->size_info[i];
            log("  %zu: bucket_size=%zu free_count=%zu desired_free_count=%zu group_count=%zu\n",
                i, info->bucket_size, info->free_count, info->desired_free_count, info->group_count);

        }
        free_list_count = CLUB_HEAP_MAX_SIZES;
    }
    log("free list:\n");
    for (size_t i = 0; i < free_list_count; i++) {
        struct heap_bucket_links *f = &heap->free_list[i];
        log("  shift %2zu (size %4x): (%p) prev=%p next=%p\n", i + HEAP_MIN_SHIFT, 1 << i, f, f->prev,
            f->next);
    }
}
#endif

static void
heap_bucket_unlink(struct heap_bucket *bucket) {
    ensure(bucket->magic == HEAP_BUCKET_SPLITTER || bucket->magic == HEAP_BUCKET_CLUB);
    ensure(!bucket->in_use);
    struct heap_bucket_links links = bucket->free_links;
#if HEAP_DEBUG
    log("heap_bucket_unlink bucket=%p prev=%p next=%p\n", bucket, links.prev, links.next);
#endif
    links.prev->next = links.next;
    links.next->prev = links.prev;
    bucket->free_links = {nullptr, nullptr};
}

static void
heap_bucket_link(struct heap *heap, struct heap_bucket *bucket) {
    struct heap_bucket_links *head = &heap->free_list[bucket->free_list_idx];
#if HEAP_DEBUG
    log("heap_bucket_link bucket=%p head=%p next=%p\n", bucket, head, head->next);
#endif
    ensure(!bucket->in_use);
    bucket->free_links.prev = head;
    bucket->free_links.next = head->next;
    head->next->prev = &bucket->free_links;
    head->next = &bucket->free_links;
}

static struct heap_bucket *
heap_bucket_split(struct heap_bucket *first) {
    uint8_t free_list_idx = first->free_list_idx, new_free_list_idx = free_list_idx - 1;
    first->free_list_idx = new_free_list_idx;
    struct heap_bucket *second = splitter_heap_bucket_next(first);
    second->magic = HEAP_BUCKET_SPLITTER;
    second->free_list_idx = new_free_list_idx;
    second->prev_free_list_idx = new_free_list_idx;
    second->in_use = false;
    second->has_next = first->has_next;
    second->pad = 0;
    first->has_next = true;
    if (second->has_next)
        splitter_heap_bucket_next(second)->prev_free_list_idx = new_free_list_idx;
#if HEAP_DEBUG
    log("split(%p) -> %p (%u->%u)\n", first, second, free_list_idx, new_free_list_idx);
#endif
    return second;
}

static void
splitter_heap_bucket_join_next(struct heap_bucket *first) {
    ensure(first->has_next);
    struct heap_bucket *second = splitter_heap_bucket_next(first);
    ensure(second->free_list_idx == first->free_list_idx);
    uint8_t new_free_list_idx = first->free_list_idx + 1;
#if HEAP_DEBUG
    log("join(%p, %p) (%u->%u)\n", first, second, first->free_list_idx, new_free_list_idx);
#endif
    first->free_list_idx = new_free_list_idx;
    if ((first->has_next = second->has_next))
        splitter_heap_bucket_next(first)->prev_free_list_idx = new_free_list_idx;
}

static struct heap_bucket *
splitter_heap_alloc_chunk(struct splitter_heap *heap) {
    size_t size = (size_t)1 << HEAP_MAX_SHIFT;
    struct heap_bucket *bucket = (struct heap_bucket *)MEMAllocFromDefaultHeapEx(size, HEAP_ALLOC_ALIGN);
    if (bucket) {
        bucket->magic = HEAP_BUCKET_SPLITTER;
        bucket->free_list_idx = HEAP_MAX_SHIFT - HEAP_MIN_SHIFT;
        bucket->prev_free_list_idx = (uint8_t)-1;
        bucket->in_use = false;
        bucket->has_next = false;
        bucket->pad = 0;
#if HEAP_DEBUG
        ensure(heap->debug_chunks_list->insert(bucket).second);
#endif
    }
    return bucket;
}

void
splitter_heap_init(struct splitter_heap *heap) {
    heap->magic = HEAP_SPLITTER;
    for (size_t i = 0; i < countof(heap->free_list_storage); i++) {
        struct heap_bucket_links *links = &heap->free_list[i];
        links->next = links->prev = links;
    }
#if HEAP_DEBUG
    heap->debug_chunks_list.reset(new std::unordered_set<struct heap_bucket *>);
#endif
}

static maybe<void *>
splitter_heap_alloc(struct splitter_heap *heap, size_t bucket_size) {
    size_t shift = max(log2_round_up(bucket_size), HEAP_MIN_SHIFT);

    struct heap_bucket *bucket;
    if (!heap || shift > HEAP_MAX_SHIFT) {
        bucket = (struct heap_bucket *)MEMAllocFromDefaultHeapEx(bucket_size, HEAP_ALLOC_ALIGN);
        if (bucket) {
#if HEAP_DEBUG
            log("->%p %p\n", bucket, bucket->data);
#endif
            bucket->magic = HEAP_BUCKET_SYSTEM;
            bucket->in_use = true;
            return just((void *)bucket->data);
        }
        return nothing;
    }
    size_t cur_shift = shift;
    while (1) {
        struct heap_bucket_links *links = &heap->free_list[cur_shift - HEAP_MIN_SHIFT];
        if (links->next != links) {
            bucket = (struct heap_bucket *)((char *)links->next
                              - offsetof(struct heap_bucket, free_links));
            heap_bucket_unlink(bucket);
            break;
        }
        if (cur_shift == HEAP_MAX_SHIFT) {
            if (!(bucket = splitter_heap_alloc_chunk(heap)))
                return nothing;
            break;
        }
        cur_shift++;
    }
    ensure(bucket->magic == HEAP_BUCKET_SPLITTER);
    while (cur_shift > shift) {
        heap_bucket_link(heap, heap_bucket_split(bucket));
        cur_shift--;
    }
    bucket->in_use = true;
    return just((void *)bucket->data);
}

static void
splitter_heap_free(struct splitter_heap *heap, struct heap_bucket *bucket) {
    while (1) {
        struct heap_bucket *prev, *next;
        if (bucket->has_next
            && (next = splitter_heap_bucket_next(bucket))->free_list_idx == bucket->free_list_idx
            && !next->in_use) {
            heap_bucket_unlink(next);
            splitter_heap_bucket_join_next(bucket);
            continue;
        }
        if (bucket->prev_free_list_idx != (uint8_t)-1
            && (prev = splitter_heap_bucket_prev(bucket))->free_list_idx == bucket->free_list_idx
            && !prev->in_use) {
            heap_bucket_unlink(prev);
            splitter_heap_bucket_join_next(prev);
            bucket = prev;
            continue;
        }
        break;
    }
    if (bucket->free_list_idx == HEAP_MAX_SHIFT - HEAP_MIN_SHIFT) {
#if HEAP_DEBUG
        log("freeing bucket %p\n", bucket);
        ensure(heap->debug_chunks_list->erase(bucket));
#endif
        MEMFreeToDefaultHeap(bucket);
    } else {
        heap_bucket_link(heap, bucket);
    }
}

bool
club_heap_init(struct club_heap *heap, struct splitter_heap *backing, size_t alloc_size, size_t est_group_count, size_t desired_free_count) {
    heap->magic = HEAP_CLUB;
    for (size_t i = 0; i < countof(heap->free_list_storage); i++) {
        struct heap_bucket_links *links = &heap->free_list[i];
        links->next = links->prev = links;
    }
    heap->backing = backing;
    struct club_heap::size_info *size_info = &heap->size_info[0];
    size_info->bucket_size = (alloc_size + offsetof(struct heap_bucket, data) + HEAP_ALLOC_ALIGN - 1) & ~(HEAP_ALLOC_ALIGN - 1);
    ensure(size_info->bucket_size >= alloc_size);
    size_info->bucket_size = max(size_info->bucket_size, sizeof(struct heap_bucket));
    size_info->free_count = 0;
    size_info->desired_free_count = desired_free_count;
    ensure(est_group_count <= 255);
    size_t est_group_bucket_size = sizeof(struct club_heap_group_header) + est_group_count * size_info->bucket_size + sizeof(struct heap_bucket);
    size_t rounded_group_bucket_size = (size_t)1 << log2_round_up(est_group_bucket_size);
    size_t group_count = (rounded_group_bucket_size - sizeof(struct heap_bucket) - sizeof(struct club_heap_group_header)) / size_info->bucket_size;
    group_count = min(group_count, (size_t)255);
    size_info->group_count = group_count;
#if HEAP_DEBUG
    log("est_group_count=%zu; actual=%zu for rounded_group_bucket_size %#zx\n", est_group_count, group_count, rounded_group_bucket_size);
    heap->debug_groups_list.reset(new std::unordered_set<struct club_heap_group_header *>);
#endif
    return club_heap_ensure_min_free_count(heap);
}

static bool
club_heap_alloc_group(struct club_heap *heap, size_t free_list_idx) {
    struct club_heap::size_info *size_info = &heap->size_info[free_list_idx];
    size_t bucket_size = size_info->bucket_size, group_count = size_info->group_count;
    size_t group_alloc_size = sizeof(struct club_heap_group_header) + bucket_size * group_count;
    void *alloc = unwrap_or(heap_alloc(heap->backing, group_alloc_size), return false);
    struct club_heap_group_header *header = (struct club_heap_group_header *)alloc;
    header->magic = CLUB_HEAP_GROUP;
    header->group_free_count = (uint16_t)group_count;
    size_info->free_count += (uint16_t)group_count;
    for (size_t i = 0; i < group_count; i++) {
        struct heap_bucket *bucket = (struct heap_bucket *)((char *)(header + 1) + i * bucket_size);
        bucket->magic = HEAP_BUCKET_CLUB;
        bucket->free_list_idx = (uint8_t)free_list_idx;
        bucket->idx_in_group = (uint8_t)i;
        bucket->in_use = false;
        bucket->has_next = false;
        bucket->pad = 0;
        heap_bucket_link(heap, bucket);
    }
#if HEAP_DEBUG
    ensure(heap->debug_groups_list->insert(header).second);
#endif
    return true;
}

static inline struct club_heap_group_header *
club_heap_bucket_get_group_header(struct club_heap *heap, struct heap_bucket *bucket) {
    auto ret = (struct club_heap_group_header *)((char *)bucket - heap->size_info[bucket->free_list_idx].bucket_size * bucket->idx_in_group - sizeof(struct club_heap_group_header));
    ensure(ret->magic == CLUB_HEAP_GROUP);
    return ret;
}

static maybe<void *>
club_heap_alloc(struct club_heap *heap, size_t bucket_size) {
    for (size_t i = 0; i < CLUB_HEAP_MAX_SIZES; i++) {
        struct club_heap::size_info *info = &heap->size_info[i];
        if (bucket_size <= info->bucket_size) {
            struct heap_bucket_links *head = &heap->free_list[i];
            if (head->next == head) {
                if (!club_heap_alloc_group(heap, i))
                    return nothing;
            }
            struct heap_bucket *bucket = (struct heap_bucket *)((char *)head->next
                              - offsetof(struct heap_bucket, free_links));
            ensure(bucket->magic == HEAP_BUCKET_CLUB);
            heap_bucket_unlink(bucket);
            bucket->in_use = true;
            ensure(info->free_count > 0);
            info->free_count--;
            club_heap_bucket_get_group_header(heap, bucket)->group_free_count--;
            return just((void *)bucket->data);
        }
    }
    return splitter_heap_alloc(heap->backing, bucket_size);
}

static void
club_heap_free(struct club_heap *heap, struct heap_bucket *bucket) {
    if (bucket->magic != HEAP_BUCKET_CLUB)
        return splitter_heap_free(heap->backing, bucket);
    heap_bucket_link(heap, bucket);
    struct club_heap_group_header *group_header = club_heap_bucket_get_group_header(heap, bucket);
    struct club_heap::size_info *size_info = &heap->size_info[bucket->free_list_idx];
    size_t bucket_size = size_info->bucket_size, group_count = size_info->group_count;
    size_info->free_count++;
    group_header->group_free_count++;
    if (size_info->free_count >= size_info->desired_free_count + group_count &&
        group_header->group_free_count == group_count) {
        // kill the group
        for (size_t i = 0; i < group_count; i++) {
            struct heap_bucket *bucket = (struct heap_bucket *)((char *)(group_header + 1) + i * bucket_size);
            heap_bucket_unlink(bucket);
            size_info->free_count--;
        }
        heap_free(heap->backing, group_header);
#if HEAP_DEBUG
        ensure(heap->debug_groups_list->erase(group_header));
#endif
    }
}

bool
club_heap_ensure_min_free_count(struct club_heap *heap) {
    for (size_t i = 0; i < CLUB_HEAP_MAX_SIZES; i++) {
        struct club_heap::size_info *info = &heap->size_info[i];
        while (info->free_count < info->desired_free_count) {
            if (!club_heap_alloc_group(heap, i))
                return false;
        }

    }
    return true;
}

[[noreturn]]
static void bad_heap(struct heap *heap) {
    panic("bad heap %p magic %d\n", heap, heap->magic);
}

maybe<void *>
heap_alloc(struct heap *heap, size_t size) {
    size_t bucket_size = sat_add(size, offsetof(struct heap_bucket, data));
    maybe<void *> ret = nothing;
    switch (heap->magic) {
    case HEAP_SPLITTER:
        ret = splitter_heap_alloc((struct splitter_heap *)heap, bucket_size);
        break;
    case HEAP_CLUB:
        ret = club_heap_alloc((struct club_heap *)heap, bucket_size);
        break;
    default:
        bad_heap(heap);
    }
    log("heap_alloc(%p, %zu) -> %p\n", heap, size, ret.ptr_or_null());
    return ret;
}

void
heap_free(struct heap *heap, void *ptr) {
    log("heap_free(%p, %p)\n", heap, ptr);
    if (!ptr)
        return;
    struct heap_bucket *bucket = (struct heap_bucket *)((char *)ptr - offsetof(struct heap_bucket, data));
    ensure(bucket->in_use);
    bucket->in_use = false;
    if (bucket->magic == HEAP_BUCKET_SYSTEM) {
        MEMFreeToDefaultHeap(bucket);
        return;
    }
    switch (heap->magic) {
    case HEAP_SPLITTER:
        return splitter_heap_free((struct splitter_heap *)heap, bucket);
    case HEAP_CLUB:
        return club_heap_free((struct club_heap *)heap, bucket);
    default:
        bad_heap(heap);
    }
}

#endif // HEAP_IS_MALLOC

maybe<void *> heap_zalloc(struct heap *heap, size_t size) {
    void *ret = unwrap_or(heap_alloc(heap, size), return nothing);
    memset(ret, 0, size);
    return just(ret);
}

void
_uarray_free(struct _uarray **array) {
    if (*array)
        heap_free((*array)->heap, *array);
}

maybe<void *>
_uarray_appendn(struct _uarray **array, size_t n, size_t elmsize, struct heap *heap) {
    size_t i = 0;
    struct _uarray *u = *array;
    if (!u || (i = u->count, n > u->capacity - i)) {
        if (!_uarray_realloc(array, sat_add(i, n), elmsize, heap))
            return nothing;
        u = *array;
    }
    u->count = i + n;
    return just((void *)(u->data + i * elmsize));
}

bool
_uarray_realloc(struct _uarray **array, size_t new_capacity, size_t elmsize, struct heap *heap) {
    if (!heap)
        return false;
    size_t count = *array ? (*array)->count : 0;
    ensure(count <= new_capacity);
    struct _uarray *u = *array;
    size_t size = sat_add(sizeof(*u), sat_mul(new_capacity, elmsize));
    struct _uarray *new_array = (struct _uarray *)unwrap_or(heap_alloc(heap, size), return false);
    new_array->heap = heap;
    new_array->capacity = new_capacity;
    new_array->count = count;
    if (count)
        memcpy(new_array->data, u->data, count * elmsize);
    _uarray_free(array);
    *array = new_array;
    return true;
}

void
_uarray_shrink(struct _uarray **array, size_t new_count) {
    if (new_count)
        ensure(*array && (*array)->count >= new_count);
    if (*array)
        (*array)->count = new_count;
}

void
_uarray_remove(struct _uarray **array, size_t i, size_t elmsize) {
    struct _uarray *u = *array;
    ensure(u && u->count > i);
    memmove(u->data + i * elmsize, u->data + (i + 1) * elmsize, (u->count - i - 1) * elmsize);
    u->count--;
}

static uint32_t
_uhash_compute_hash(uintptr_t key) {
    uint32_t h = (uint32_t)key; // ...change this if this is ever ported to Switch, lol
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

maybe<struct _uhash_bucket *>
_uhash_find(struct _uhash_data *h, uintptr_t key, size_t bucketsize) {
    //log("_uhash_find(key=%lu)\n", key);
    if (!h)
        return nothing;
    size_t capacity = h->capacity;
    size_t start = _uhash_compute_hash(key) % capacity;
    for (size_t i = 0; ; i++) {
        struct _uhash_bucket *bucket = (struct _uhash_bucket *)(h->buckets + ((start + i) % capacity) * bucketsize);
        size_t their_dist = ((start + i) - bucket->ideal_pos + capacity) % capacity;
        //log("_uhash_find(key=%lu): probing %zu, present=%d their_dst=%zu i=%zu\n", key, (start + i) % capacity, bucket->present, their_dist, i);
        if (!bucket->ever_present || their_dist > i)
            return nothing;
        if (bucket->present && bucket->key == key)
            return just(bucket);
    }
}

maybe<struct _uhash_bucket *>
_uhash_insert(struct _uhash *hash, uintptr_t orig_key, size_t bucketsize) {
    //log("_uhash_insert(key=%lu)\n", orig_key);
    struct _uhash_data *h = hash->data;
    if (!h || ++h->count >= (size_t)((uint64_t)h->capacity * 9 / 10)) {
        // resize
        size_t capacity = h ? h->capacity : 0;
        size_t new_capacity = h ? sat_mul(capacity, 2) : 16;
        struct heap *heap = hash->heap;
        struct _uhash_data *new_h = (struct _uhash_data *)unwrap_or(heap_zalloc(heap, offsetof(struct _uhash_data, buckets) + sat_mul(new_capacity, bucketsize)), {
            return nothing;
        });
        new_h->capacity = new_capacity;
        for (size_t i = 0; i < capacity; i++) {
            struct _uhash_bucket *bucket = (struct _uhash_bucket*)(h->buckets + i * bucketsize);
            if (bucket->present) {
                struct _uhash fake = {new_h, (struct heap *)0x123};
                maybe<struct _uhash_bucket *> new_bucket = _uhash_insert(&fake, bucket->key, bucketsize);
                memcpy(new_bucket.unwrap()->padding_and_value, bucket->padding_and_value, bucketsize - offsetof(struct _uhash_bucket, padding_and_value));
            }
        }
        heap_free(heap, h);
        h = hash->data = new_h;
        ++h->count;
    }
    size_t capacity = h->capacity;
    size_t start = _uhash_compute_hash(orig_key) % capacity;
    struct _uhash_bucket *bucket_to_return = nullptr;
    char pending_bucket[2][bucketsize] __attribute__((aligned(8)));
    size_t pending_bucket_idx = 0;
    size_t dist = 0;
    size_t i = start;
    while (1) {
        struct _uhash_bucket *bucket = (struct _uhash_bucket *)(h->buckets + i * bucketsize);
        size_t their_dist = (i - bucket->ideal_pos + capacity) % capacity;
        bool should_swap = their_dist < i;
        if (!bucket->ever_present || (should_swap && !bucket->present)) {
            if (bucket_to_return) {
                //log("btr: copying to %zu\n", i);
                memcpy(bucket, pending_bucket[pending_bucket_idx], bucketsize);
                return just(bucket_to_return);
            } else {
                bucket->ideal_pos = (uint32_t)start;
                bucket->key = orig_key;
                bucket->present = bucket->ever_present = 1;
                return just(bucket);
            }
        } else if (should_swap) {
            size_t new_pending_bucket_idx = (pending_bucket_idx + 1) & 1;
            size_t new_start = bucket->ideal_pos;
            memcpy(pending_bucket[new_pending_bucket_idx], bucket, bucketsize);
            if (bucket_to_return) {
                //log("copying to %zu\n", i);
                memcpy(bucket, pending_bucket[pending_bucket_idx], bucketsize);
            } else {
                bucket->ideal_pos = (uint32_t)start;
                bucket->key = orig_key;
                bucket->present = bucket->ever_present = 1;
                bucket_to_return = bucket;
            }
            pending_bucket_idx = new_pending_bucket_idx;
            start = new_start;
            dist = their_dist;
        }
        i = (i + 1) % capacity;
        dist++;
    }
}

struct _uhash_find_or_insert_ret
_uhash_find_or_insert(struct _uhash *hash, uintptr_t key, size_t bucketsize) {
    maybe<struct _uhash_bucket *> ret = _uhash_find(hash->data, key, bucketsize);
    if (ret)
        return {ret, true};
    ret = _uhash_insert(hash, key, bucketsize);
    return {ret, false};
}

void
_uhash_debug(struct _uhash *hash, size_t bucketsize) {
    if (!hash->data) {
        log("[uhash is null]\n");
        return;
    }
    log("count=%zd\n", hash->data->count);
    char *buckets = hash->data->buckets;
    for (size_t i = 0; i < hash->data->capacity; i++) {
        struct _uhash_bucket *bucket = (struct _uhash_bucket *)(buckets + i * bucketsize);
        log("[%zu]: (%p) key=%#lx hash=%#x present=%d ever_present=%d\n", i, bucket, bucket->key, bucket->ideal_pos, bucket->present, bucket->ever_present);
    }
}

static inline uintptr_t
shift_left(uintptr_t val, int n) {
    return n >= (8 * sizeof(uintptr_t)) ? 0 : (val << n);
}
static inline int
clz_uintptr(uintptr_t val) {
#if DUMMY
    if (val == 0)
        return 8 * sizeof(uintptr_t);
#endif
    return __builtin_clzl(val);
}

maybe<struct _utrie_leaf *>
_utrie_find(struct _utrie *restrict ut, uintptr_t key, size_t insert_alloc_size, bool *is_new) {
    struct _utrie_elem **elem = &ut->root;
    struct _utrie_node *parent = nullptr;
    bool may_insert = is_new != nullptr;
    while (1) {
        struct _utrie_elem *elem_val = *elem;
        //log("_utrie_find: parent=%p elem=%p elem_val=%p\n", parent, elem, elem_val);
        if (elem_val == nullptr) {
            if (likely(!may_insert))
                return nothing;
            struct _utrie_leaf *insert_leaf = (struct _utrie_leaf *)unwrap_or(heap_zalloc(ut->heap, insert_alloc_size), {
                return nothing;
            });
            insert_leaf->base.key = key;
            insert_leaf->base.parent_shr_1 = (uintptr_t)parent >> 1;
            insert_leaf->base.is_node = 0;
            *elem = &insert_leaf->base;
            if (is_new)
                *is_new = true;
            return just(insert_leaf);
        }
        //log("_utrie_find: key=%#lx (mine=%#lx) node=%d\n", elem_val->key, key, elem_val->is_node);
        uintptr_t other_key = elem_val->key;
        int min_array_lobit;
        if (!elem_val->is_node) {
            if (other_key == key) {
                if (is_new)
                    *is_new = false;
                return just((struct _utrie_leaf *)elem_val);
            }
            min_array_lobit = 0;
        } else {
            struct _utrie_node *node = (struct _utrie_node *)elem_val;
            //log("_utrie_find: node pfx=%d ary=%d\n", node->prefix_lobit, node->array_lobit);
            uintptr_t mask = shift_left((uintptr_t)-1, node->prefix_lobit);
            if (!((key ^ other_key) & mask)) {
                uint8_t array_bitlen = node->prefix_lobit - node->array_lobit;
                uintptr_t my_idx = (key >> node->array_lobit) & (((uintptr_t)1 << array_bitlen) - 1);
                elem = &node->elems[my_idx];
                parent = node;
                continue;
            }
            min_array_lobit = node->prefix_lobit;
        }
        // otherwise, alloc subnode
        if (likely(!may_insert))
            return nothing;
        int highest_different_bit = (int)(8 * sizeof(uintptr_t)) - 1 - clz_uintptr(key ^ other_key);
        int max_prefix_lobit = highest_different_bit + 1;
        // Go as high as possible (makes the logic simpler)
        // TODO: try harder to avoid creating small holes
        int prefix_lobit = max_prefix_lobit;
        int array_lobit = max(prefix_lobit - UTRIE_WIDTH_SHIFT, min_array_lobit);
        struct _utrie_node *new_node = (struct _utrie_node *)unwrap_or(heap_zalloc(ut->heap, sizeof(*new_node)), {
            return nothing;
        });
        new_node->base.is_node = 1;
        new_node->base.parent_shr_1 = (uintptr_t)parent >> 1;
        new_node->base.key = key;
        new_node->prefix_lobit = (uint8_t)prefix_lobit;
        new_node->array_lobit = (uint8_t)array_lobit;
        int new_array_bitlen = prefix_lobit - array_lobit;
        debug_ensure(new_array_bitlen >= 0 && new_array_bitlen <= UTRIE_WIDTH_SHIFT);
        debug_ensure(array_lobit >= 0 && array_lobit < 8 * sizeof(uintptr_t));
        uintptr_t other_new_idx = (other_key >> array_lobit) & (((uintptr_t)1 << new_array_bitlen) - 1);
        new_node->elems[other_new_idx] = elem_val;
        *elem = &new_node->base;
        elem_val->parent_shr_1 = (uintptr_t)new_node >> 1;
        parent = new_node;
        continue;
    }
}

static inline struct _utrie_elem **
_utrie_node_get_elem_pointing_to_me(struct _utrie *ut, struct _utrie_elem *elem) {
    if (elem->parent_shr_1 == 0x42)
        panic("probably trying to erase already-erased elem");
    struct _utrie_node *parent = (struct _utrie_node *)(elem->parent_shr_1 << 1);
    uintptr_t key = elem->key;
    struct _utrie_elem **ret;
    if (parent == nullptr) {
        ret = &ut->root;
    } else {
        uint8_t array_bitlen = parent->prefix_lobit - parent->array_lobit;
        uintptr_t idx = (key >> parent->array_lobit) & (((uintptr_t)1 << array_bitlen) - 1);
        ret = &parent->elems[idx];
    }
    ensure_eq(*ret, elem);
    return ret;
}

void
_utrie_erase(struct _utrie *ut, struct _utrie_leaf *leaf) {
    struct _utrie_node *parent = (struct _utrie_node *)(leaf->base.parent_shr_1 << 1);
    struct _utrie_elem **elem = _utrie_node_get_elem_pointing_to_me(ut, &leaf->base);
    *elem = nullptr;
    leaf->base.parent_shr_1 = 0x42;
    if (parent != nullptr) {
        struct _utrie_elem *only_elem = nullptr;
        for (size_t i = 0; i < UTRIE_WIDTH; i++) {
            struct _utrie_elem *other_elem = parent->elems[i];
            if (other_elem != nullptr) {
                if (only_elem != nullptr)
                    return; // more than 1
                else
                    only_elem = other_elem;
            }
        }
        // only one element, collapse
        ensure(only_elem != nullptr);
        //log("collapsing %p\n", parent);
        struct _utrie_node *parpar = (struct _utrie_node *)(parent->base.parent_shr_1 << 1);
        struct _utrie_elem **parent_elem = _utrie_node_get_elem_pointing_to_me(ut, &parent->base);
        *parent_elem = only_elem;
        only_elem->parent_shr_1 = (uintptr_t)parpar >> 1;
        heap_free(ut->heap, parent);
    }
}

static void
_utrie_debug_elem(struct _utrie *ut, struct _utrie_elem **elem, struct _utrie_node *expected_parent, int max_prefix_lobit, int indent) {
    struct _utrie_elem *elem_val = *elem;
    if (elem_val == nullptr) {
        log("\n");
    } else {
        struct _utrie_node *parent = (struct _utrie_node *)(elem_val->parent_shr_1 << 1);
        log("%p: key=%#lx, parent=%p |  ", elem_val, elem_val->key, parent);
        ensure_eq(expected_parent, parent);
        if (elem_val->is_node) {
            struct _utrie_node *node = (struct _utrie_node *)elem_val;
            size_t len = (size_t)1 << (node->prefix_lobit - node->array_lobit);
            log("node: pfx=%d ary=%d [len=%zd]\n", node->prefix_lobit, node->array_lobit, len);
            ensure(node->prefix_lobit <= max_prefix_lobit);
            ensure(node->array_lobit < node->prefix_lobit);
            indent += 3;
            for (size_t i = 0; i < UTRIE_WIDTH; i++) {
                if (i == len)
                    log("%*s--\n", indent, "");
                log("%*s[%zu]: ", indent, "", i);
                _utrie_debug_elem(ut, &node->elems[i], node, node->prefix_lobit - 1, indent);
                if (i >= len)
                    ensure_eq(node->elems[i], nullptr);
            }
        } else {
            log("leaf\n");
        }
    }
}
void
_utrie_debug(struct _utrie *ut) {
    log("_utrie %p root: ", ut);
    _utrie_debug_elem(ut, &ut->root, nullptr, 8 * sizeof(uintptr_t), 0);
}

static inline maybe<struct _utrie_leaf *>
_utrie_descend(struct _utrie_elem *elem_val) {
    again: if (elem_val && elem_val->is_node) {
        struct _utrie_node *node = (struct _utrie_node *)elem_val;
        for (size_t i = 0; i < UTRIE_WIDTH; i++) {
            if (node->elems[i] != nullptr) {
                elem_val = node->elems[i];
                goto again;
            }
        }
    }
    return maybe<struct _utrie_leaf *>((struct _utrie_leaf *)elem_val);
}

static inline maybe<struct _utrie_leaf *>
_utrie_descend_last(struct _utrie_elem *elem_val) {
    again: if (elem_val && elem_val->is_node) {
        struct _utrie_node *node = (struct _utrie_node *)elem_val;
        for (size_t i = UTRIE_WIDTH - 1; i != (size_t)-1; i--) {
            if (node->elems[i] != nullptr) {
                elem_val = node->elems[i];
                goto again;
            }
        }
    }
    return maybe<struct _utrie_leaf *>((struct _utrie_leaf *)elem_val);
}

maybe<struct _utrie_leaf *>
_utrie_first_ge(struct _utrie *ut, uintptr_t cmp) {
    struct _utrie_elem *elem_val = ut->root;
    if (!elem_val)
        return nothing;
    if (!elem_val->is_node)
        return elem_val->key >= cmp ? just((struct _utrie_leaf *)elem_val) : nothing;
    struct _utrie_node *node = (struct _utrie_node *)elem_val;
    again: {
        uint8_t array_bitlen = node->prefix_lobit - node->array_lobit;
        size_t array_len = 1 << array_bitlen;
        size_t slot_for_cmp = (cmp >> node->array_lobit) & (array_len - 1);
        uintptr_t mask = shift_left((uintptr_t)-1, node->prefix_lobit);
        uintptr_t theirs = node->base.key & mask;
        uintptr_t mine = cmp & mask;
        //log("node:%p theirs=%lx mine=%lx\n", node, theirs, mine);
        if (mine < theirs)
            return _utrie_descend(&node->base);
        else if (mine > theirs)
            goto next_of_last;
        else {
            for (size_t i = slot_for_cmp; i < array_len; i++) {
                struct _utrie_elem *sub_val = node->elems[i];
                if (sub_val == nullptr) {
                    continue;
                } else if (sub_val->is_node) {
                    node = (struct _utrie_node *)sub_val;
                    goto again;
                } else {
                    auto leaf = (struct _utrie_leaf *)sub_val;
                    if (sub_val->key >= cmp)
                        return just(leaf);
                }
            }
            // no?
        next_of_last:
            //log("next<dl<%p>=%p>\n", &node->base, _utrie_descend_last(&node->base).unwrap());
            return _utrie_next(_utrie_descend_last(&node->base).unwrap());
        }
    }
}

maybe<struct _utrie_leaf *>
_utrie_first(struct _utrie *ut) {
    return _utrie_descend(ut->root);
}

maybe<struct _utrie_leaf *>
_utrie_next(struct _utrie_leaf *leaf) {
    struct _utrie_elem *elem_val = &leaf->base;
    while (1) {
        struct _utrie_node *parent = (struct _utrie_node *)(elem_val->parent_shr_1 << 1);
        if (parent == nullptr)
            return nothing;
        ensure(parent->base.is_node);
        uint8_t array_bitlen = parent->prefix_lobit - parent->array_lobit;
        size_t array_len = 1 << array_bitlen;
        size_t i = (elem_val->key >> parent->array_lobit) & (array_len - 1);
        //log("_utrie_next: parent=%p i=%zu\n", parent, i);
        while (1) {
            ++i;
            //log("i=%zu\n", i);
            if (i == array_len) {
                elem_val = &parent->base;
                break;
            } else {
                elem_val = parent->elems[i];
                if (elem_val != nullptr)
                    return _utrie_descend(elem_val);
            }
        }
    }
}

#if CONTAINERTEST
#include <unordered_set>
#include <unordered_map>
#include <vector>
#include <map>
#include <sys/random.h>

static void
test_basics() {

    ensure(log2_round_up(0x12) == 5);
    ensure(log2_round_up(0x20) == 5);
    ensure(log2_round_up(1) == 0);
    ensure(log2_round_up(0) == 0); // *shrug*
    ensure(log2_round_up(-1ul) == 8 * sizeof(size_t));

    uarray<callback<int(int)>> callbacks;
    for (int i = 0; i < 10; i++)
        ensure(callbacks.append([i](int x) { return x * i; }, nullptr));
    ensure_eq(callbacks[5](6), 30);

    {
        struct foo { foo() {} int val = 0; };
        callback<void(foo, foo &, foo &&)> modes_test = [](foo a, foo &b, foo &&c) {
            a.val = b.val = c.val = 1;
        };
        foo a, b, c;
        modes_test(a, b, move(c));
        ensure(a.val == 0);
        ensure(b.val == 1);
        ensure(c.val == 1);
    }
}

static void
test_heap() {
    ensure(!HEAP_IS_MALLOC);
    bool use_club = getchar() & 1;
    size_t club_alloc_size = (size_t)getw(stdin) % 4096;
    size_t club_est_group_count = 1 + ((size_t)getw(stdin) % 50);
    struct splitter_heap sheap;
    splitter_heap_init(&sheap);
    struct club_heap cheap;
    struct heap *heap;
    if (use_club) {
        club_heap_init(&cheap, &sheap, club_alloc_size, club_est_group_count, 0);
        heap = &cheap;
    } else
        heap = &sheap;

    struct alloc_info {
        void *buf;
        size_t alloc_size;
        char fill_char;
    };
    std::vector<struct alloc_info> allocs;
    while (!feof(stdin)) {
        bool should_alloc = (allocs.size() == 0) | (getchar() & 1);
        if (should_alloc) {
            size_t alloc_bits = (size_t)getchar() % 18;
            size_t alloc_size = (size_t)getw(stdin) % (1 << alloc_bits);
            char fill_char = (char)getchar();
            if ((getchar() & 1) && use_club)
                alloc_size = club_alloc_size;
            void *buf = heap_alloc(heap, alloc_size).unwrap();
            memset(buf, fill_char, alloc_size);
            if (HEAP_DEBUG)
                log("alloc: allocs[%zu] = %p\n", allocs.size(), buf);
            allocs.push_back({buf, alloc_size, fill_char});
        } else {
            size_t which = (size_t)getw(stdin) % allocs.size();
            struct alloc_info ai = allocs[which];
            if (HEAP_DEBUG)
                log("free: allocs[%zu] = %p\n", which, ai.buf);
            for (size_t i = 0; i < ai.alloc_size; i++)
                ensure(((char *)ai.buf)[i] == ai.fill_char);
            heap_free(heap, ai.buf);
            struct alloc_info other = allocs.back();
            allocs.pop_back();
            if (which < allocs.size())
                allocs[which] = other;
        }
    }
#if HEAP_DEBUG
    heap_debug(heap);
#endif
}

static void
test_hash_2() {
    struct splitter_heap hash_heap;
    splitter_heap_init(&hash_heap);
    uhash<uint32_t, uint64_t> hash;
    hash.init(&hash_heap);
    hash.set(0x235ff140, 123);
    hash.set(0x2371b408, 123);
    hash.set(0x2371cc00, 123);
    hash.set(0x23720cd0, 123);
    hash.erase(hash.get(0x23720cd0).unwrap());
    hash.set(0x23722f48, 123);
    hash.set(0x23722fd0, 123);
    hash.debug();
    hash.set(0x237258a0, 123);
    hash.debug();
    hash.erase(hash.get(0x237258a0).unwrap());
    hash.debug();
    hash.erase(hash.get(0x23722fd0).unwrap());
    hash.destroy();
}

static void
test_hash() {
    static std::unordered_map<uint64_t, ssize_t> test_val_refcount;
    test_val_refcount.clear();
    constexpr bool LOUD = false, TV_LOUD = false;
    struct test_val {
        uint64_t val;
        uint64_t uniqid;
        test_val() {
            init(0);
            if (TV_LOUD)
                log("!construct %p default\n", this);
        }
        test_val(test_val &&other) {
            init(other.val);
            if (TV_LOUD)
                log("+copyconstruct %p from %p: %llx\n", this, &other, val);
        }
        test_val(uint64_t val) {
            init(val);
            if (TV_LOUD)
                log("+construct %p %llx\n", this, val);
        }
        test_val &operator=(test_val &&other) {
            val = other.val;
            if (TV_LOUD)
                log("copyassign %p %llx\n", this, val);
            return *this;
        }
        void init(uint64_t val) {
            this->val = val;
            ensure_errno(!getentropy(&this->uniqid, sizeof(this->uniqid)));
            test_val_refcount[this->uniqid]++;
        }
        ~test_val() {
            test_val_refcount[uniqid]--;
            if (TV_LOUD)
                log("-destruct %p %llx\n", this, val);
        }
    };
    struct splitter_heap hash_heap;
    splitter_heap_init(&hash_heap);
    {
        uhash<uint16_t, struct test_val> hash;
        hash.init(&hash_heap);
        std::unordered_map<uint16_t, uint64_t> expected;
        while (1) {
            int mode = getchar();
            if (mode == EOF)
                break;
            switch (mode & 1) {
            case 0: { // insert
                uint16_t key = 0;
                uint64_t val = 0;
                fread(&key, sizeof(key), 1, stdin);
                fread(&val, sizeof(val), 1, stdin);
                if (LOUD)
                    log("insert %#x %llx\n", key, val);
                hash.set(key, val);
                if (LOUD)
                    hash.debug();
                expected[key] = val;
                break;
            }
            case 1: { // lookup
                uint16_t key = 0;
                fread(&key, sizeof(key), 1, stdin);
                if (LOUD)
                    log("lookup %#x\n", key);
                bool erase = getchar() & 1;
                auto it = expected.find(key);
                auto val = hash.get(key);
                if (it != expected.end()) {
                    ensure(val);
                    ensure(val.unwrap()->val == it->second);
                    if (erase) {
                        if (LOUD)
                            log("erase\n");
                        hash.erase(val.unwrap());
                        expected.erase(it);
                    }
                } else {
                    ensure(!val);
                }
                break;
            }
            }
        }

        if (LOUD)
            hash.debug();

        std::unordered_set<uint16_t> seen_keys, expected_keys;
        for (auto bucket : hash) {
            ensure(seen_keys.insert(bucket->key).second);
            auto it = expected.find(bucket->key);
            if (it == expected.end())
                panic("failed to find key %d in expected\n", bucket->key);
            ensure(bucket->val.val == it->second);
        }
        for (auto bucket : expected)
            ensure(expected_keys.insert(bucket.first).second);
        ensure(seen_keys == expected_keys);
        hash.destroy();
    }
    bool refcount_bad = false;
    for (auto bucket : test_val_refcount) {
        if (bucket.second != 0) {
            log("still alive: %llx (%zd)\n", bucket.first, bucket.second);
            refcount_bad = true;
        }
    }
    ensure(!refcount_bad);
}

static void
test_trie() {
    struct splitter_heap trie_heap;
    splitter_heap_init(&trie_heap);
    struct test_item {
        struct _utrie_leaf leaf;
        int number;
    };
    utrie<struct test_item> ut = {&trie_heap};
    std::map<uintptr_t, struct test_item *> expected;
    while (1) {
        int etc = getchar();
        if (etc == EOF)
            break;
        uintptr_t key = 0;
        int val = 0;
        fread(&key, sizeof(key), 1, stdin);
        fread(&val, sizeof(val), 1, stdin);
        if ((etc & 0x30) == 0x10) {
            //log("first >= %#lx\n", key);
            //ut.debug();
            maybe<struct test_item *> ptr = ut.first_ge(key);
            auto it = expected.lower_bound(key);
            ensure_eq(it != expected.end(), !!ptr);
            if (ptr)
                ensure_eq(it->second, ptr.unwrap());
            continue;
        }
        bool should_insert = etc & 0x1;

        auto it = expected.find(key);

        bool is_new;
        auto leaf = ut.find(key, should_insert ? &is_new : nullptr);
        if (it != expected.end()) {
            ensure(leaf);
            struct test_item *ti = leaf.unwrap();
            ensure(ti->leaf.base.key == key);
            ensure(ti->number == 42);
            ensure(it->first == key);
            ensure(it->second == ti);
            if (!(etc & 0xe)) {
                // delete
                ut.erase(ti);
                expected.erase(it);
            }
        } else {
            if (should_insert) {
                ensure(leaf);
                ensure(is_new);
                struct test_item *ti = leaf.unwrap();
                ensure(ti->leaf.base.key == key);
                ti->number = 42;
                expected[key] = ti;
            } else {
                ensure(!leaf);
            }
        }
    }
    std::unordered_set<struct test_item *> seen_items, expected_items;
    for (auto bucket : expected) {
        //log("expected: %p\n", bucket.second);
        ensure(expected_items.insert(bucket.second).second);
    }
    for (struct test_item *ti : ut) {
        //log("seen: %p\n", leaf);
        ensure(seen_items.insert(ti).second);
    }
    //utrie_debug(&ut);
    ensure(seen_items == expected_items);
    for (auto bucket : expected) {
        //utrie_debug(&ut);
        //log("erasing %p\n", bucket.second);
        ut.erase(bucket.second);
        heap_free(&trie_heap, bucket.second);
    }
    //utrie_debug(&ut);
}

#ifdef __AFL_LOOP
#define WHILE_AFL_LOOP while (__AFL_LOOP(1000))
#else
#define WHILE_AFL_LOOP
#endif

int
main(int argc, char **argv) {
    WHILE_AFL_LOOP {
        char *mode = argv[1];
        if (!mode)
            goto bad;
        if (!strcmp(mode, "test-heap")) {
            test_heap();
        } else if (!strcmp(mode, "test-hash")) {
            test_hash();
        } else if (!strcmp(mode, "test-hash-2")) {
            test_hash_2();
        } else if (!strcmp(mode, "test-trie")) {
            test_trie();
        } else if (!strcmp(mode, "test-basics")) {
            test_basics();
        } else {
            bad: panic("bad argument");
        }
    }
}
#endif // CONTAINERTEST

END_LOCAL_DECLS
