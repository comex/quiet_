#include "trace.h"
#include "decls.h"
#include "ssprintf.h"
#include "logging.h"
#include "misc.h"
#include "ssscanf.h"

enum agent_op : uint8_t {
    agent_op_float = 0x01,
    agent_op_add = 0x02,
    agent_op_sub = 0x03,
    agent_op_mul = 0x04,
    agent_op_div_signed = 0x05,
    agent_op_div_unsigned = 0x06,
    agent_op_rem_signed = 0x07,
    agent_op_rem_unsigned = 0x08,
    agent_op_lsh = 0x09,
    agent_op_rsh_signed = 0x0a,
    agent_op_rsh_unsigned = 0x0b,
    agent_op_trace = 0x0c,
    agent_op_trace_quick = 0x0d,
    agent_op_log_not = 0x0e,
    agent_op_bit_and = 0x0f,
    agent_op_bit_or = 0x10,
    agent_op_bit_xor = 0x11,
    agent_op_bit_not = 0x12,
    agent_op_equal = 0x13,
    agent_op_less_signed = 0x14,
    agent_op_less_unsigned = 0x15,
    agent_op_ext = 0x16,
    agent_op_ref8 = 0x17,
    agent_op_ref16 = 0x18,
    agent_op_ref32 = 0x19,
    agent_op_ref64 = 0x1a,
    agent_op_ref_float = 0x1b,
    agent_op_ref_double = 0x1c,
    agent_op_ref_long_double = 0x1d,
    agent_op_l_to_d = 0x1e,
    agent_op_d_to_l = 0x1f,
    agent_op_if_goto = 0x20,
    agent_op_goto = 0x21,
    agent_op_const8 = 0x22,
    agent_op_const16 = 0x23,
    agent_op_const32 = 0x24,
    agent_op_const64 = 0x25,
    agent_op_reg = 0x26,
    agent_op_end = 0x27,
    agent_op_dup = 0x28,
    agent_op_pop = 0x29,
    agent_op_zero_ext = 0x2a,
    agent_op_swap = 0x2b,
    agent_op_getv = 0x2c,
    agent_op_setv = 0x2d,
    agent_op_tracev = 0x2e,
    agent_op_tracenz = 0x2f,
    agent_op_trace16 = 0x30,
    agent_op_invalid2 = 0x31,
    agent_op_pick = 0x32,
    agent_op_rot = 0x33,
    agent_op_printf = 0x34,
};

// a good 256 bytes of stack, but this is on the gdbstub thread
constexpr size_t GDBSTUB_AGENT_MAX_STACK = 32;

constexpr size_t GDBSTUB_AGENT_MAX_RESULTS = 2;

struct validator_agent_ctx {
    static constexpr bool is_evaluator = false;
    const uint8_t *nbyte_arg(size_t n) {
        const uint8_t *ret = &expr[pc];
        while (n--) {
            if (end == pc) {
                expect(false, "arg hits end");
                return nullptr;
            }
            if (expected_stack_len_by_pc[pc] != 0xff)
                expect(false, "branched to arg");
            pc++;
        }
        return ret;
    }
    void push(uint64_t dummy) {
        if (stack_len == GDBSTUB_AGENT_MAX_STACK)
            expect(false, "stack overflow");
        else
            stack_len++;
    }
    uint64_t pop() {
        if (stack_len == 0)
            expect(false, "stack underflow");
        else
            stack_len--;
        return 0;
    }

    __attribute__((format(printf, 3, 4)))
    void expect(bool cond, const char *fmt, ...) {
        if (!cond) {
            my_va_list map;
            MVA_START(&map, fmt);
            if (!had_error) {
                log("gdbstub_validate_agent_expr: validation failed (start=%#zx, pc=%#zx): `i%s%p\n", start, pc, fmt, &map);
                had_error = true;
            }
        }
    }
    bool keep_going() {
        if (pc == end) {
            if (!got_end)
                expect(false, "pc runs off end");
            return false;
        }
        check_stack_len(pc);
        uint8_t expected = expected_stack_len_by_pc[pc];
        if (expected != 0xff && expected != stack_len)
            expect(false, "stack len conflict");
        return !had_error;
    }
    void validate_offset(uint16_t offset) {
        if (offset < pc - start || offset >= end - start)
            expect(false, "invalid jump offset (offset=%#x, end=%zx)", offset, end);
        else
            check_stack_len(start + offset);
    }
    void check_stack_len(size_t loc) {
        uint8_t expected = expected_stack_len_by_pc[loc];
        if (expected == 0xff)
            expected_stack_len_by_pc[loc] = (uint8_t)stack_len;
        else if (expected != stack_len)
            expect(false, "stack len conflict at pc %#zx, had %u, now have %zu", loc, expected, stack_len);
    }
    uint64_t *var_ptr(uint16_t which) {
        vars_needed = max(vars_needed, (size_t)which + 1);
        return nullptr;
    }

    bool got_end;
    size_t stack_len;
    bool had_error;
    const uint8_t *expr;
    size_t start, pc, end;
    size_t vars_needed = 0;
    uint8_t *expected_stack_len_by_pc;
    bool for_trace_experiment;

    size_t results_count;
    uint64_t results[GDBSTUB_AGENT_MAX_RESULTS];
};

struct evaluator_agent_ctx {
    static constexpr bool is_evaluator = true;
    inline const uint8_t *nbyte_arg(size_t n) {
        const uint8_t *ret = &expr[pc];
        pc += n;
        return ret;
    }
    inline void push(uint64_t val) {
        stack[stack_len++] = val;
    }
    inline void expect(bool cond, const char *fmt, ...) {}
    inline uint64_t pop() {
        return stack[--stack_len];
    }
    inline bool keep_going() {
        return !got_end;
    }
    inline void validate_offset(uint16_t offset) {}
    inline uint64_t *var_ptr(uint16_t which) {
        return &vars[which];
    }

    uint64_t *stack, *vars;
    size_t stack_len;
    bool got_end;
    const uint8_t *expr;
    size_t start, pc, end;
    struct gdbstub_thread *gthread;
    struct gdbstub_trace_experiment *trace_experiment;

    size_t results_count;
    uint64_t results[GDBSTUB_AGENT_MAX_RESULTS];
};

static UNUSED inline void
gdbstub_trace_experiment_start_frame(struct gdbstub_trace_experiment *experiment, uint16_t tracepoint_idx) {
    struct gdb_trace_frame_header frame = {tracepoint_idx, 0};
    if (circ_buf_avail_space(&experiment->trace_buffer) < sizeof(frame)) {
        experiment->did_overflow = true;
        return;
    }
    ensure(circ_buf_push_bytes(&experiment->trace_buffer, &frame, sizeof(frame)) == sizeof(frame));
    ensure(!experiment->trace_buffer_cur_frame_len);
    experiment->trace_buffer_cur_frame_len = sizeof(frame);

}

static inline void
gdbstub_trace_experiment_push_block(struct gdbstub_trace_experiment *experiment, const void *buf, size_t size) {
    {
        if (!experiment->trace_buffer_cur_frame_len)
            return; // already overflowed
        if (size > experiment->trace_buffer.cap - experiment->trace_buffer_cur_frame_len)
            goto overflow;
        while (circ_buf_avail_space(&experiment->trace_buffer) < size) {
            if (!experiment->circular_mode)
                goto overflow;
            struct gdb_trace_frame_header frame;
            ensure(circ_buf_shift_bytes(&experiment->trace_buffer, &frame, sizeof(frame)) == sizeof(frame));
            ensure(circ_buf_shift_bytes(&experiment->trace_buffer, nullptr, frame.len) == frame.len);
        }
        ensure(circ_buf_push_bytes(&experiment->trace_buffer, buf, size) == size);
        return;
    }

    overflow: {
        experiment->did_overflow = true;
        experiment->trace_buffer.len -= experiment->trace_buffer_cur_frame_len;
        experiment->trace_buffer_cur_frame_len = 0;
        return;
    }
}

static UNUSED inline void
gdbstub_trace_experiment_end_frame(struct gdbstub_trace_experiment *experiment) {
    size_t frame_len = experiment->trace_buffer_cur_frame_len;
    if (!frame_len)
        return; // already overflowed
    struct circ_buf dummy = experiment->trace_buffer;
    dummy.len -= frame_len;
    dummy.len += offsetof(struct gdb_trace_frame_header, len);
    uint32_t len_to_write = (uint32_t)(frame_len - sizeof(struct gdb_trace_frame_header));
    ensure(circ_buf_push_bytes(&dummy, &len_to_write, sizeof(len_to_write)) == sizeof(len_to_write));
    experiment->trace_buffer_cur_frame_len = 0;
}

template <typename Ctx>
static inline bool
gdbstub_eval_agent_expr_with_ctx(Ctx *ctx, struct gdbstub *gs, const struct gdbstub_agent_expr *restrict expr) {
    ctx->expr = expr->expr;
    ctx->end = expr->len;
    ctx->pc = 0;
    ctx->stack_len = 0;

    auto binop = [&](auto f) {
        UNUSED uint64_t a = ctx->pop();
        UNUSED uint64_t b = ctx->pop();
        uint64_t ret = 0;
        if constexpr(Ctx::is_evaluator)
            ret = f(a, b);
        ctx->push(ret);
    };
    auto binop_signed = [&](auto f) {
        binop([&](uint64_t a, uint64_t b) {
            int64_t signed_ret = f((int64_t)a, (int64_t)b);
            return (uint64_t)signed_ret;
        });
    };
    auto unop = [&](auto f) {
        UNUSED uint64_t a = ctx->pop();
        uint64_t ret = 0;
        if constexpr(Ctx::is_evaluator)
            ret = f(a);
        ctx->push(ret);
    };
    auto uN_arg = [&](auto val) -> decltype(val) {
        const uint8_t *ptr = ctx->nbyte_arg(sizeof(val));
        if (Ctx::is_evaluator || ptr)
            memcpy(&val, ptr, sizeof(val));
        return bswap(val);
    };
    auto u8_arg = [&]() -> uint8_t { return uN_arg((uint8_t)0); };
    auto u16_arg = [&]() -> uint16_t { return uN_arg((uint16_t)0); };
    auto u32_arg = [&]() -> uint32_t { return uN_arg((uint32_t)0); };
    auto u64_arg = [&]() -> uint64_t { return uN_arg((uint64_t)0); };

    uint64_t trace_addr, trace_size;
    UNUSED bool trace_nz;

    ctx->got_end = false;
    while (ctx->keep_going()) {
        ctx->got_end = false;
        ctx->start = ctx->pc;
        uint8_t op = expr->expr[ctx->pc++];
        switch ((enum agent_op)op) {
        case agent_op_add:
            binop([](uint64_t a, uint64_t b) { return a + b; }); break;
        case agent_op_sub:
            binop([](uint64_t a, uint64_t b) { return a - b; }); break;
        case agent_op_mul:
            binop([](uint64_t a, uint64_t b) { return a * b; }); break;
        case agent_op_div_unsigned:
            binop([](uint64_t a, uint64_t b) { return b ? (a / b) : 0; }); break;
        //case agent_op_rem_unsigned:
        //    binop([](uint64_t a, uint64_t b) { return b ? (a % b) : 0; }); break;
        case agent_op_div_signed:
            binop_signed([](int64_t a, int64_t b) { return
                (a == INT64_MIN & b == -1) ? INT64_MIN :
                (b == 0) ? 0 :
                (a / b);
            }); break;
        //case agent_op_rem_signed:
        //    binop_signed([](int64_t a, int64_t b) { return
        //        (a == INT64_MIN & b == -1) ? 0 :
        //        (b == 0) ? 0 :
        //        (a % b);
        //    }); break;
        case agent_op_lsh:
            binop([](uint64_t a, uint64_t b) { return b >= 64 ? 0 : (a << b); }); break;
        case agent_op_rsh_signed:
            binop_signed([](int64_t a, int64_t b) { return b >= 64 ? 0 : (a << b); }); break;
        case agent_op_rsh_unsigned:
            binop([](uint64_t a, uint64_t b) { return b >= 64 ? 0 : (a >> b); }); break;
        // trace and trace_quick are below
        case agent_op_log_not:
            unop([](uint64_t a) { return !a; }); break;
        case agent_op_bit_and:
            binop([](uint64_t a, uint64_t b) { return a & b; }); break;
        case agent_op_bit_or:
            binop([](uint64_t a, uint64_t b) { return a | b; }); break;
        case agent_op_bit_xor:
            binop([](uint64_t a, uint64_t b) { return a ^ b; }); break;
        case agent_op_bit_not:
            unop([](uint64_t a) { return ~a; }); break;
        case agent_op_equal:
            binop([](uint64_t a, uint64_t b) { return a == b; }); break;
        case agent_op_less_signed:
            binop_signed([](int64_t a, int64_t b) { return a < b; }); break;
        case agent_op_less_unsigned:
            binop([](uint64_t a, uint64_t b) { return a < b; }); break;
        case agent_op_ext: {
            uint8_t bits = u8_arg();
            ctx->expect(bits >= 0 && bits < 64, "ext: invalid arg");
            unop([&](uint64_t a) {
                return (uint64_t)((int64_t)(a << (64 - bits)) >> (64 - bits));
            });
            break;
        }
        case agent_op_zero_ext: { // out of order
            uint8_t bits = u8_arg();
            ctx->expect(bits >= 0 && bits < 64, "zero_ext: invalid arg");
            unop([&](uint64_t a) {
                return (a << (64 - bits)) >> (64 - bits);
            });
            break;
        }
        case agent_op_ref8:
        case agent_op_ref16:
        case agent_op_ref32:
        case agent_op_ref64: {
            size_t read_len = 1 << (op - agent_op_ref8);
            uint64_t addr = ctx->pop();
            if constexpr(Ctx::is_evaluator) {
                if (addr > (uint32_t)-1) {
                    log("gdbstub_eval_agent_expr_with_ctx: overflowing read address %#llx\n", addr);
                    return false;
                }
                uint64_t val = 0;
                size_t actual = gdbstub_read_mem(gs, (char *)&val + (8 - read_len), (uint32_t)addr, read_len);
                if (actual != read_len) {
                    log("gdbstub_eval_agent_expr_with_ctx: read failed at (%#llx, %zu)\n", addr, read_len);
                    return false;
                }
                ctx->push(val);
            } else
                ctx->push(0);
            break;
        }
        case agent_op_if_goto: {
            uint16_t offset = u16_arg();
            UNUSED uint64_t test = ctx->pop();
            ctx->validate_offset(offset);
            if constexpr(Ctx::is_evaluator) {
                if (test)
                    ctx->pc = ctx->start + offset;
            }
            break;
        }
        case agent_op_goto: {
            uint16_t offset = u16_arg();
            ctx->validate_offset(offset);
            if constexpr(Ctx::is_evaluator) {
                ctx->pc = ctx->start + offset;
            }
            break;
        }
        case agent_op_const8:
            ctx->push(u8_arg());
            break;
        case agent_op_const16:
            ctx->push(u16_arg());
            break;
        case agent_op_const32:
            ctx->push(u32_arg());
            break;
        case agent_op_const64: {
            ctx->push(u64_arg());
            break;
        }
        case agent_op_reg: {
            uint16_t reg = u16_arg();
            ctx->expect(reg < GDBSTUB_REG_COUNT, "invalid register number %u", reg);
            uint64_t val = 0;
            if constexpr(Ctx::is_evaluator) {
                val = gdbstub_get_reg(gs, ctx->gthread, (enum gdbstub_reg)reg);
            }
            ctx->push(val);
            break;
        }
        case agent_op_end:
            ctx->got_end = true;
            for (size_t i = 0; i < ctx->results_count; i++)
                ctx->results[i] = ctx->pop();
            break;
        case agent_op_dup: {
            uint64_t val = ctx->pop();
            ctx->push(val);
            ctx->push(val);
            break;
        }
        case agent_op_pop:
            ctx->pop();
            break;
        // agent_op_zero_ext is above
        case agent_op_swap: {
            uint64_t a = ctx->pop(), b = ctx->pop();
            ctx->push(b); ctx->push(a);
            break;
        }
        case agent_op_getv: {
            uint64_t *p = ctx->var_ptr(u16_arg());
            ctx->push(Ctx::is_evaluator ? *p : 0);
            break;
        }
        case agent_op_setv: {
            uint64_t *p = ctx->var_ptr(u16_arg());
            uint64_t val = ctx->pop();
            if (Ctx::is_evaluator)
                *p = val;
            ctx->push(val);
            break;
        }
        case agent_op_trace: {
            trace_size = ctx->pop();
            trace_addr = ctx->pop();
            trace_nz = false;
            goto trace_mem;
        }
        case agent_op_tracenz: {
            trace_size = ctx->pop();
            trace_addr = ctx->pop();
            trace_nz = true;
            goto trace_mem;
        }
        case agent_op_trace_quick: {
            trace_size = u8_arg();
            trace_addr = ctx->pop();
            trace_nz = false;
            ctx->push(trace_addr);
            goto trace_mem;
        }
        case agent_op_trace16: {
            trace_size = u16_arg();
            trace_addr = ctx->pop();
            trace_nz = false;
            ctx->push(trace_addr);
            goto trace_mem;
        }
        trace_mem:
            if constexpr(Ctx::is_evaluator) {
                if (trace_addr > (uint32_t)-1 || trace_size > (uint16_t)-1) {
                    log("gdbstub_eval_agent_expr_with_ctx: overflowing read (%#llx, %llu)\n", trace_addr, trace_size);
                    return false;
                }
                struct gdb_trace_block_M *block = &ctx->trace_experiment->trace_block_M;
                size_t actual = gdbstub_read_mem(gs, block->data, (uint32_t)trace_addr, (size_t)trace_size);
                bool short_read_ok = false;
                size_t cut = actual;
                if (trace_nz) {
                    cut = strnlen(block->data, actual);
                    if (cut < actual)
                        short_read_ok = true;
                }
                if (actual < trace_size && !short_read_ok)
                    log("gdbstub_eval_agent_expr_with_ctx: read of (%#lx, %zu) only got %zu bytes\n", (uintptr_t)trace_addr, (size_t)trace_size, actual);
                block->magic = 'M';
                block->address = trace_addr;
                block->length = (uint16_t)cut;
                gdbstub_trace_experiment_push_block(ctx->trace_experiment, block, offsetof(struct gdb_trace_block_M, data) + cut);
            } else {
                ctx->expect(ctx->for_trace_experiment, "trace opcode in non-trace agent expr");
            }
            break;
        case agent_op_tracev: {
            uint16_t which = u16_arg();
            UNUSED uint64_t *p = ctx->var_ptr(which);
            if constexpr(Ctx::is_evaluator) {
                struct gdb_trace_block_V block = {'V', which, *p};
                gdbstub_trace_experiment_push_block(ctx->trace_experiment, &block, sizeof(block));
            } else {
                ctx->expect(ctx->for_trace_experiment, "trace opcode in non-trace agent expr");
            }
            break;
        }
        case agent_op_pick: {
            uint8_t idx = u8_arg();
            ctx->expect(idx < ctx->stack_len, "pick: invalid arg");
            if constexpr(Ctx::is_evaluator)
                ctx->push(ctx->stack[ctx->stack_len - 1 - idx]);
            else
                ctx->push(0);
            break;
        }
        case agent_op_rot: {
            uint64_t c = ctx->pop(), b = ctx->pop(), a = ctx->pop();
            ctx->push(c); ctx->push(a); ctx->push(b);
            break;
        }
        case agent_op_printf: {
            UNUSED uint8_t numargs = u8_arg();
            UNUSED uint16_t fmt_len = u16_arg();
            const char *fmt = (const char *)ctx->nbyte_arg(fmt_len);
            ctx->expect(fmt_len > 0 && fmt[fmt_len - 1] == '\0', "format string is not nul-terminated");
            ctx->expect(numargs <= 16, "too many printf args");
            UNUSED uint64_t function = ctx->pop();
            UNUSED uint64_t channel = ctx->pop();
            for (size_t i = 0; i < numargs; i++)
                ctx->pop();
            if constexpr(Ctx::is_evaluator) {
                my_va_list map = my_va_list::custom(ctx->stack + ctx->stack_len, numargs);
                log("[f=%llu/c=%llu] `i%s%p\n", function, channel, fmt, &map);
            }
            break;
        }
        case agent_op_float:
        case agent_op_ref_float:
        case agent_op_ref_double:
        case agent_op_ref_long_double:
        case agent_op_l_to_d:
        case agent_op_d_to_l:
        case agent_op_invalid2:
        default: // comment this out to have compiler check completeness
            ctx->expect(false, "invalid opcode");
        }
        // split into instructions for easier manual reading
        if constexpr(GDBSTUB_VERBOSE && !Ctx::is_evaluator) {
            char tmp[32];
            size_t insn_len = ctx->pc - ctx->start;
            ensure(insn_len <= sizeof(tmp) / 2);
            s_hex_encode(tmp, (const char *)expr->expr + ctx->start, insn_len);
            log("[%04zx] %.*s\n", ctx->start, (int)(2 * insn_len), tmp);
        }
    }

    return true;
}

bool
gdbstub_validate_agent_expr(struct gdbstub *gs, const struct gdbstub_agent_expr *restrict expr, struct gdbstub_trace_global_ctx *gctx, bool for_trace_experiment, size_t results_count) {
    struct validator_agent_ctx validator;
    validator.for_trace_experiment = for_trace_experiment;
    validator.results_count = results_count;
    validator.expected_stack_len_by_pc = (uint8_t *)unwrap_or(heap_alloc(&gs->top_heap, expr->len), {
        log("gdbstub_validate_agent_expr: oom\n");
        return false;
    });
    memset(validator.expected_stack_len_by_pc, 0xff, expr->len);
    validator.had_error = false;
    gdbstub_eval_agent_expr_with_ctx(&validator, gs, expr);
    heap_free(&gs->top_heap, validator.expected_stack_len_by_pc);
    if (validator.had_error)
        return false;
    if (validator.vars_needed > gctx->vars.count()) {
        size_t extra = gctx->vars.count() - validator.vars_needed;
        uint64_t *ptr = unwrap_or(gctx->vars.appendn(extra, &gs->top_heap), {
            log("gdbstub_validate_agent_expr: unable to allocate space for %zu more variables\n", extra);
            return false;
        });
        memset(ptr, 0, extra * sizeof(uint64_t));
    }
    return true;
}

bool
gdbstub_eval_agent_expr(struct gdbstub *gs, const struct gdbstub_agent_expr *restrict expr, struct gdbstub_trace_global_ctx *gctx, struct gdbstub_trace_experiment *trace_experiment, uint64_t *results, size_t results_count, struct gdbstub_thread *gthread) {
    struct evaluator_agent_ctx evaluator;
    uint64_t stack[GDBSTUB_AGENT_MAX_STACK];
    evaluator.stack = stack;
    evaluator.vars = gctx->vars.vals();
    evaluator.gthread = gthread;
    evaluator.trace_experiment = trace_experiment;
    evaluator.results_count = results_count;
    bool ret = gdbstub_eval_agent_expr_with_ctx(&evaluator, gs, expr);
    if (ret)
        memcpy(results, evaluator.results, results_count * sizeof(uint64_t));
    return ret;
}

#if TRACETEST
uint64_t
gdbstub_get_reg(struct gdbstub *gs, struct gdbstub_thread *gthread,
                enum gdbstub_reg which) {
    uint64_t ret = (uint64_t)which;
    log("dummy gdbstub_get_reg(%d) -> %#llx\n", which, ret);
    return ret;
}

size_t
gdbstub_read_mem(struct gdbstub *gs, char *buf, uintptr_t addr, size_t len) {
    log("dummy gdbstub_read_mem(%#lx, %#zx)\n", addr, len);
    memset(buf, 'a', len);
    return len;
}

int
main(int argc, char **argv) {
    union {
        struct gdbstub_agent_expr expr;
        struct {
            size_t len;
            char buf[8192];
        };
    } expr_buf;
    expr_buf.expr.len = 0;
    while (!feof(stdin) && !ferror(stdin)) {
        ensure(expr_buf.len < sizeof(expr_buf.buf));
        expr_buf.len += fread(expr_buf.buf + expr_buf.len, 1, sizeof(expr_buf.buf) - expr_buf.len, stdin);
    }
    struct gdbstub fakegs;
    splitter_heap_init(&fakegs.top_heap);
    struct gdbstub_trace_global_ctx gctx = {};
    constexpr size_t results_count = 1;
    ensure(gdbstub_validate_agent_expr(&fakegs, &expr_buf.expr, &gctx, false, results_count));
    uint64_t results[results_count];
    ensure(gdbstub_eval_agent_expr(&fakegs, &expr_buf.expr, &gctx, nullptr, results, results_count, nullptr));
    for (size_t i = 0; i < results_count; i++)
        log("results[%zu]=%llx\n", i, results[i]);
}
#endif
