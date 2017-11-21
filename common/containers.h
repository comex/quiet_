#pragma once
#include "decls.h"
#include "logging.h"
#include "misc.h"
#include "types.h"

BEGIN_LOCAL_DECLS

template <typename T>
struct may_be_maybed;

struct nothing_t {};
constexpr nothing_t nothing;

template <typename T>
union TRIVIAL_ABI maybe {
    __attribute__((always_inline))
    inline operator bool() { return dummy != 0; }
    __attribute__((always_inline))
    T *unwrap_ref() {
        ensure(*this);
        return &val;
    }
    template <typename = typename enable_if<__has_trivial_copy(T)>::type>
    __attribute__((always_inline))
    inline T unwrap() {
        if (!*this)
            panic("unwrap fail");
        return val;
    }

    __attribute__((always_inline))
    constexpr inline T ptr_or_null() {
        return val;
    }
    template <typename U>
    __attribute__((always_inline))
    constexpr inline maybe<U> cast() {
        return maybe<U>((U)val);
    }

    constexpr explicit inline maybe(T val) : val(move(val)) {}

    constexpr inline maybe(nothing_t) : dummy(0) {}

    inline ~maybe() {
        if (dummy > dummy_val_)
            val.~T();
    }

    // this version is explicit and its body explicitly casts
    template <typename X>
    explicit inline maybe(maybe<X> other) {
        val = move((T)other.val);
    }

    // this version is not explicit and its body requires an implicit cast
    template <typename X>
    inline operator maybe<X>() {
        return maybe<X>(val);
    }

    T val;
    uintptr_t dummy;

    static constexpr uintptr_t dummy_val_ = 1;

private:
    template <typename X>
    friend maybe<X> just(X val);
    maybe() {}
};

template <typename T>
__attribute__((always_inline))
static inline maybe<T>
just(T val) {
    may_be_maybed<T>::verify(&val);
    maybe<T> self;
    self.val = move(val);
    return self;
}

template <typename T>
struct may_be_maybed<T *> {
    __attribute__((always_inline))
    static void verify(T **ptr) {
        ensure(*ptr != nullptr);
    }
};

#define unwrap_or(maybe_expr, action...)                                              \
    ({                                                                                   \
        auto _maybe_expr = (maybe_expr);                                    \
        int _maybe_ub;                                                                \
        if (!_maybe_expr) {                                                    \
            action;                                                                      \
            _maybe_ub++; /* trigger warning for UB */                                 \
        }                                                                                \
        _maybe_expr.unwrap(); \
    })

constexpr size_t HEAP_MIN_SHIFT =
    sizeof(void *) == 8 ? 5 : 4; // enough for struct heap_bucket
constexpr size_t HEAP_MAX_SHIFT = 15; // 32kb
constexpr size_t HEAP_ALLOC_ALIGN = 8;

struct heap_bucket_links {
    struct heap_bucket_links *prev;
    struct heap_bucket_links *next;
};

enum heap_magic : uint8_t {
    HEAP_BUCKET_SPLITTER = 0x10,
    HEAP_BUCKET_SYSTEM,
    HEAP_BUCKET_CLUB,

    HEAP_SPLITTER = 0x20,
    HEAP_CLUB,

    CLUB_HEAP_GROUP = 0x30,
};

struct heap_bucket {
    union {
        struct {
            enum heap_magic magic;
            uint8_t free_list_idx;
            union {
                uint8_t prev_free_list_idx; // for splitter
                uint8_t idx_in_group; // for club
            };
            bool in_use : 1;
            bool has_next : 1; // only for splitter
            uint8_t pad : 6;
            char _header_end[0];
        };
        struct {
            char _fl_header[4];
            struct heap_bucket_links free_links; // if free
        };
        struct {
            char _d_header[4];
            char data[0] __attribute__((aligned(HEAP_ALLOC_ALIGN)));
        };
    };
};
static_assert(offsetof(struct heap_bucket, _header_end) == 4);
static_assert(!(offsetof(struct heap_bucket, data) & (HEAP_ALLOC_ALIGN - 1)));

#define HEAP_DEBUG (CONTAINERTEST && 0)

#if HEAP_DEBUG
#include <memory>
#include <unordered_set>
#endif

struct heap {
    enum heap_magic magic;
    struct heap_bucket_links free_list[0];
};

struct splitter_heap : public heap {
    struct heap_bucket_links free_list_storage[HEAP_MAX_SHIFT - HEAP_MIN_SHIFT + 1];
#if HEAP_DEBUG
    std::unique_ptr<std::unordered_set<struct heap_bucket *>> debug_chunks_list;
#endif
};

constexpr size_t CLUB_HEAP_MAX_SIZES = 1;

struct club_heap_group_header {
    enum heap_magic magic;
    uint16_t group_free_count;
} __attribute__((aligned(HEAP_ALLOC_ALIGN)));


struct club_heap : public heap {
    struct heap_bucket_links free_list_storage[CLUB_HEAP_MAX_SIZES];
    struct size_info {
        size_t bucket_size; // alloc size + bucket header size
        size_t free_count;
        size_t desired_free_count;
        size_t group_count;
    } size_info[CLUB_HEAP_MAX_SIZES];
    struct splitter_heap *backing;
#if HEAP_DEBUG
    std::unique_ptr<std::unordered_set<struct club_heap_group_header *>> debug_groups_list;
#endif
};

void splitter_heap_init(struct splitter_heap *heap);
bool club_heap_init(struct club_heap *heap, struct splitter_heap *backing, size_t alloc_size, size_t est_group_count, size_t desired_free_count);
BEGIN_LOCAL_DECLS
maybe<void *> heap_alloc(struct heap *heap, size_t size);
maybe<void *> heap_zalloc(struct heap *heap, size_t size);
END_LOCAL_DECLS
void heap_free(struct heap *heap, void *ptr);
#if HEAP_DEBUG
void heap_debug(struct heap *heap);
#endif
bool club_heap_ensure_min_free_count(struct club_heap *heap);

struct _uarray {
    struct heap *heap;
    size_t count;
    size_t capacity;
    char data[];
};

maybe<void *>
_uarray_appendn(struct _uarray **array, size_t n, size_t elmsize, struct heap *heap);

// TODO make this behave better with heap
bool _uarray_realloc(struct _uarray **array, size_t new_capacity,
                    size_t elmsize, struct heap *heap);

void _uarray_shrink(struct _uarray **array, size_t new_count);

void _uarray_remove(struct _uarray **array, size_t i, size_t elmsize);


void _uarray_free(struct _uarray **array);

template <typename T>
struct uarray {
    inline T *vals() { return backing_ ? (T *)backing_->data : nullptr; }
    inline size_t count() const { return backing_ ? backing_->count : 0; }
    inline size_t capacity() const { return backing_ ? backing_->capacity : 0; }

    __attribute__((warn_unused_result))
    inline maybe<T *>
    append(T val, struct heap *heap) {
        T *ptr = (T *)unwrap_or(_uarray_appendn(&backing_, 1, sizeof(T), heap), return nothing);
        new (ptr) T(move(val));
        return just(ptr);
    }

    inline maybe<T *>
    appendn(size_t n, struct heap *heap) {
        T *ptr = (T *)unwrap_or(_uarray_appendn(&backing_, n, sizeof(T), heap), return nothing);
        return just(ptr);
    }

    inline void shrink(size_t new_count) {
        _uarray_shrink(&backing_, new_count);
    }
    inline bool realloc(size_t new_capacity, struct heap *heap) {
        return _uarray_realloc(&backing_, new_capacity, sizeof(T), heap);
    }

    inline T &operator[](size_t i) {
        size_t count = this->count();
        if (count <= i)
            panic("uarray::operator[] index out of range: %zu, count=%zu", i, count);
        return vals()[i];
    }
    inline T remove(size_t i) {
        T val = move((*this)[i]);
        _uarray_remove(&backing_, i, sizeof(T));
        return val;
    }
    inline T &only() {
        ensure(backing_ && backing_->count == 1);
        return ((T *)backing_->data)[0];
    }

    inline T *begin() { return vals(); }
    inline T *end() { return vals() + count(); }

    inline void clear() {
        _uarray_free(&backing_);
        backing_ = nullptr;
    }

    constexpr inline uarray() : backing_(nullptr) {}
    inline ~uarray() {
        _uarray_free(&backing_);
    }
    inline uarray(uarray &&other) : backing_(other.backing_) {
        other.backing_ = nullptr;
    }
    inline uarray &operator=(uarray &&other) {
        _uarray_free(&backing_);
        backing_ = other.backing_;
        other.backing_ = nullptr;
        return *this;
    }

    struct _uarray *backing_;
};

#if WANT_UHASH || ENABLE_MODULES
struct _uhash_data {
    size_t count, capacity;
    char buckets[] __attribute__((aligned(8)));
};
struct _uhash {
    struct _uhash_data *data;
    struct heap *heap;
};
struct _uhash_bucket {
    uintptr_t key;
    uint32_t ideal_pos:30, present:1, ever_present:1;
    char padding_and_value[0];
};

maybe<struct _uhash_bucket *> _uhash_find(struct _uhash_data *h, uintptr_t key, size_t bucketsize);
maybe<struct _uhash_bucket *> _uhash_insert(struct _uhash *hash, uintptr_t orig_key, size_t bucketsize);
void _uhash_debug(struct _uhash *hash, size_t bucketsize);

struct _uhash_find_or_insert_ret {
    maybe<struct _uhash_bucket *> bucket;
    bool found;
};
struct _uhash_find_or_insert_ret
_uhash_find_or_insert(struct _uhash *hash, uintptr_t key, size_t bucketsize);

template <typename Key, typename Val>
struct uhash {
    static_assert(sizeof(Key) <= sizeof(uintptr_t));
    static_assert(alignof(Key) <= 8);
    static_assert(alignof(Val) <= 8);
    static_assert(__has_trivial_destructor(Key));
    constexpr inline void init(struct heap *heap) {
        backing.data = nullptr;
        backing.heap = heap;
    }
    inline void destroy() {
        if constexpr(!__has_trivial_destructor(Val)) {
            for (struct bucket *bucket : *this)
                bucket->val.~Val();
        }
    }

    struct bucket {
        union {
            struct _uhash_bucket base;
            Key key;
        };
        Val val;
    };

    struct iterator {
        struct _uhash_data *h;
        size_t i;

        inline struct iterator &operator++() {
            do {
                //log("++%zu\n", i);
                ++i;
            } while (i < h->capacity && !((struct bucket *)h->buckets)[i].base.present);
            //log("done at %zu\n", i);
            return *this;
        }
        inline bool operator!=(iterator &other) const {
            return i != other.i;
        }
        inline struct bucket *operator*() const {
            return ((struct bucket *)h->buckets) + i;
        }
    };

    struct iterator begin() {
        struct iterator it = {backing.data, 0};
        if (it.h) {
            it.i = (size_t)-1;
            ++it;
        }
        return it;
    }
    inline struct iterator end() {
        return {backing.data, (size_t)(backing.data ? backing.data->capacity : 0)};
    }

    __attribute__((always_inline))
    inline uintptr_t
    key_to_uintptr(const Key &key) {
        uintptr_t u = 0;
        memcpy(&u, &key, sizeof(key));
        return u;
    }

    __attribute__((always_inline))
    inline maybe<Val *>
    get(Key key) {
        maybe<struct _uhash_bucket *> ret = _uhash_find(backing.data, key_to_uintptr(key), sizeof(struct bucket));
        if (ret)
            return just(&((struct bucket *)ret.unwrap())->val);
        else
            return nothing;
    }

    struct find_or_insert_ret {
        maybe<Val *> val;
        bool found;
    };

    __attribute__((always_inline))
    inline struct find_or_insert_ret
    find_or_insert(Key key) {
        struct _uhash_find_or_insert_ret ret = _uhash_find_or_insert(&backing, key_to_uintptr(key), sizeof(struct bucket));
        return {
            ret.bucket ? maybe<Val *>(&((struct bucket *)ret.bucket.unwrap())->val) : nothing,
            ret.found
        };
    }

    __attribute__((always_inline))
    inline void
    erase(Val *val) {
        struct bucket *bucket = (struct bucket *)((char *)val - offsetof(struct bucket, val));
        bucket->val.~Val();
        bucket->base.present = 0;
    }

    __attribute__((always_inline))
    inline bool
    set(Key key, Val val) {
        struct _uhash_find_or_insert_ret ret = _uhash_find_or_insert(&backing, key_to_uintptr(key), sizeof(struct bucket));
        if (ret.bucket) {
            struct bucket *b = (struct bucket *)ret.bucket.unwrap();
            if (ret.found)
                b->val = move(val);
            else
                new (&b->val) Val(move(val));
            return true;
        } else {
            return false; // OOM
        }
    }

    void debug() { _uhash_debug(&backing, sizeof(struct bucket)); }

    struct _uhash backing;
};
#endif // WANT_UHASH

#if WANT_UTRIE || ENABLE_MODULES
static constexpr uint8_t UTRIE_WIDTH_SHIFT = 3;
static constexpr size_t UTRIE_WIDTH = 1 << UTRIE_WIDTH_SHIFT;

struct _utrie;

struct _utrie_elem {
    uintptr_t key;
    uintptr_t parent_shr_1:(8 * sizeof(uintptr_t) - 1),
        is_node:1;
};

struct _utrie_node {
    struct _utrie_elem base;
    uint8_t prefix_lobit;
    uint8_t array_lobit;
    struct _utrie_elem *elems[UTRIE_WIDTH];
};

struct _utrie_leaf {
    struct _utrie_elem base;
    // ...
};

maybe<struct _utrie_leaf *> _utrie_find(struct _utrie *restrict ut, uintptr_t key, size_t insert_alloc_size, bool *is_new);
void _utrie_erase(struct _utrie *ut, struct _utrie_leaf *leaf);
void _utrie_debug(struct _utrie *ut);

maybe<struct _utrie_leaf *> _utrie_first_ge(struct _utrie *restrict ut, uintptr_t cmp);
maybe<struct _utrie_leaf *> _utrie_first(struct _utrie *ut);
maybe<struct _utrie_leaf *> _utrie_next(struct _utrie_leaf *leaf);

struct _utrie {
    struct heap *heap;
    struct _utrie_elem *root;
};

static inline void
_utrie_init(struct _utrie *restrict ut, struct heap *heap) {
    ut->heap = heap;
    ut->root = nullptr;
}

template <typename T>
struct utrie {
    static_assert(offsetof(T, leaf) == 0);
    using _as = __assert_types_equal<decltype(((T *)0)->leaf), struct _utrie_leaf>;
    // v-- helpers
    struct iterator {
        maybe<T *> leaf;
        maybe<T *> next;
        INLINE iterator &operator++() {
            leaf = next;
            if (leaf)
                next = utrie::next(leaf.unwrap());
            return *this;
        }
        INLINE bool operator!=(iterator other) {
            return leaf != other.leaf;
        }
        INLINE T *operator*() {
            return leaf.unwrap();
        }
    };
    INLINE struct iterator begin() {
        iterator it{nothing, first()};
        ++it;
        return it;
    }
    INLINE struct iterator end() {
        return iterator{nothing, nothing};
    }
    INLINE maybe<T *> first() {
        return (maybe<T *>)_utrie_first(&backing);
    }
    INLINE maybe<T *> first_ge(uintptr_t cmp) {
        return (maybe<T *>)_utrie_first_ge(&backing, cmp);
    }
    static INLINE maybe<T *> next(T *leaf) {
        return (maybe<T *>)_utrie_next(&leaf->leaf);
    }
    INLINE maybe<T *> find(uintptr_t key, bool *is_new) {
        return (maybe<T *>)_utrie_find(&backing, key, is_new != nullptr ? sizeof(T) : 0, is_new);
    }
    INLINE void erase(T *leaf) { _utrie_erase(&backing, &leaf->leaf); }
    INLINE void debug() { _utrie_debug(&backing); }
    INLINE void init(struct heap *heap) {
        _utrie_init(&backing, heap);
    }

    struct _utrie backing;
};

#endif // WANT_UTRIE

#if WANT_CALLBACK || ENABLE_MODULES
template <typename FuncTy>
struct callback;
template <typename Ret, typename... Args>
struct TRIVIAL_ABI callback<Ret(Args...)> {
    template <typename F>
    callback(F func) {
        static_assert(sizeof(F) <= sizeof(storage_), "sizeof(storage)");
        static_assert(alignof(F) <= alignof(decltype(storage_)), "alignof(storage)");
        static_assert(__has_trivial_destructor(F), "__has_trivial_destructor(F)");
        // should be trivially movable, but can't check that!
        F *fp = (F *)storage_;
        new (fp) F(move(func));
        fptr_ = wrapper<F>;
    }

    callback(callback &&move) {
        memcpy(this, &move, sizeof(*this));
    }
    callback &operator=(callback &&move) {
        memcpy(this, &move, sizeof(*this));
        return *this;
    }
    callback(callback &copy) = delete;
    callback &operator=(callback &copy) = delete;

    callback() {
        fptr_ = NULL;
    }
    template <typename F>
    static Ret wrapper(callback *self, Args... args) {
        return (*((F *)self->storage_))((Args)args...);
    }

    Ret operator()(Args... args) {
        return fptr_(this, (Args)args...);
    }

    Ret (*fptr_)(callback *, Args...);
    uintptr_t storage_[2];
};
#endif // WANT_CALLBACK

template <typename Func>
struct make_iterable {
    struct end_t {};
    using Val = decltype((*(Func *)0)().unwrap());

    make_iterable(Func &&func) : func(func), cur(nothing) {}
    make_iterable &begin() {
        cur = func();
        return *this;
    }
    end_t end() { return end_t{}; }
    INLINE bool operator!=(end_t end) { return !!cur; }
    INLINE Val &operator*() {
        return *cur.unwrap_ref();
    }
    INLINE void operator++() {
        cur = func();
    }

    Func func;
    maybe<Val> cur;
};

END_LOCAL_DECLS
