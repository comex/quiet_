#pragma once
#include "types.h"
#include "containers.h"

BEGIN_LOCAL_DECLS

constexpr int GDBSTUB_VERBOSE = 0;

#if GDBSTUB_CPP
struct gdbstub_base
#else
struct gdbstub
#endif
{
    struct splitter_heap top_heap;
};

#define _enum_repeat32(x)                                                                \
    x##0, x##1, x##2, x##3, x##4, x##5, x##6, x##7, x##8, x##9, x##10, x##11, x##12,     \
        x##13, x##14, x##15, x##16, x##17, x##18, x##19, x##20, x##21, x##22, x##23,     \
        x##24, x##25, x##26, x##27, x##28, x##29, x##30, x##31

enum gdbstub_reg {
    _enum_repeat32(GDBSTUB_REG_GPR),
    _enum_repeat32(GDBSTUB_REG_FPR),
    GDBSTUB_REG_PC = 64,
    GDBSTUB_REG_MSR,
    GDBSTUB_REG_CR,
    GDBSTUB_REG_LR,
    GDBSTUB_REG_CTR,
    GDBSTUB_REG_XER,
    GDBSTUB_REG_FPSCR = 70,
    GDBSTUB_REG_COUNT
};

// all regs are 32-bit except 64-bit FPR0..31
constexpr size_t GDBSTUB_REG_BUF_SIZE = 4 * GDBSTUB_REG_COUNT + 4 * 32;

uint64_t gdbstub_get_reg(struct gdbstub *gs, struct gdbstub_thread *gthread,
                         enum gdbstub_reg which);
size_t gdbstub_read_mem(struct gdbstub *gs, char *buf, uint32_t addr, size_t len);

END_LOCAL_DECLS
