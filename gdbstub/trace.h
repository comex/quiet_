#pragma once
#include "types.h"
#include "gdbstub.h"
#include "containers.h"
#include "circ_buf.h"

struct gdbstub_agent_expr {
    size_t len;
    uint8_t expr[];
};

struct gdb_trace_frame_header {
    uint16_t tracepoint_idx;
    uint32_t len; // not including header
} __attribute__((packed));

struct gdb_trace_block_R {
    char magic;
    char regs[GDBSTUB_REG_BUF_SIZE];
};
struct gdb_trace_block_M {
    char magic;
    uint64_t address;
    uint16_t length;
    char data[65536];
} __attribute__((packed));

struct gdb_trace_block_V {
    char magic;
    uint16_t which;
    uint64_t value;
} __attribute__((packed));

struct gdbstub_trace_global_ctx {
    uarray<uint64_t> vars;
};

struct gdbstub_trace_experiment {
    struct circ_buf trace_buffer;
    size_t trace_buffer_cur_frame_len; // including header
    bool circular_mode;
    bool did_overflow;
    struct gdb_trace_block_M trace_block_M; // temp buffer for read
};

bool gdbstub_validate_agent_expr(struct gdbstub *gs, const struct gdbstub_agent_expr *restrict expr, struct gdbstub_trace_global_ctx *gctx, bool for_trace_experiment, size_t results_count);

bool gdbstub_eval_agent_expr(struct gdbstub *gs, const struct gdbstub_agent_expr *restrict expr, struct gdbstub_trace_global_ctx *gctx, struct gdbstub_trace_experiment *trace_experiment, uint64_t *restrict results, size_t results_count, struct gdbstub_thread *gthread);
