#pragma once
#include "decls.h"
#include "misc.h"
#include "logging.h"

struct circ_buf {
    char *data;
    size_t start, len, cap;
};

static inline void
circ_buf_init(struct circ_buf *cb, void *data, size_t cap) {
    cb->start = cb->len = 0;
    cb->data = (char *)data;
    cb->cap = cap;
}

static inline size_t
circ_buf_avail_space(const struct circ_buf *cb) {
    return cb->cap - cb->len;
}

static inline void
circ_buf_push_start(struct circ_buf *cb, void **outp, size_t *capp) {
    if (cb->len == 0)
        cb->start = 0;
    size_t off = (cb->start + cb->len) % cb->cap;
    *outp = cb->data + off;
    if (off >= cb->start)
        *capp = cb->cap - off;
    else
        *capp = cb->start - off;
}

static inline void
circ_buf_push_finish(struct circ_buf *cb, size_t len) {
    cb->len += len;
}

static inline size_t
circ_buf_push_bytes(struct circ_buf *cb, const void *in, size_t in_len) {
    size_t actual = 0;
    for (size_t i = 0; i < 2 && actual < in_len; i++) {
        void *out;
        size_t this_len;
        circ_buf_push_start(cb, &out, &this_len);
        this_len = min(this_len, in_len - actual);
        memcpy(out, (char *)in + actual, this_len);
        actual += this_len;
        circ_buf_push_finish(cb, this_len);
    }
    return actual;
}

static inline void
circ_buf_shift_start(struct circ_buf *cb, const void **inp, size_t *capp) {
    *inp = cb->data + cb->start;
    *capp = min(cb->len, cb->cap - cb->start);
}

static inline void
circ_buf_shift_finish(struct circ_buf *cb, size_t len) {
    cb->start = (cb->start + len) % cb->cap;
    ensure(cb->len >= len);
    cb->len -= len;
}

static inline size_t
circ_buf_shift_bytes(struct circ_buf *cb, void *out, size_t out_len) {
    size_t actual = 0;
    for (size_t i = 0; i < 2 && actual < out_len; i++) {
        const void *in;
        size_t this_len;
        circ_buf_shift_start(cb, &in, &this_len);
        this_len = min(this_len, out_len - actual);
        if (out != nullptr)
            memcpy(out, (char *)out + actual, this_len);
        actual += this_len;
        circ_buf_shift_finish(cb, this_len);
    }
    return actual;
}

