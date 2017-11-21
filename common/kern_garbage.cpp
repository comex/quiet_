#include "kern_garbage.h"
#include "logging.h"
#include "misc.h"
#include "types.h"
#include "decls.h"

struct bat {
    uint32_t u;
    uint32_t l;
};

#define NUM_IBATnU(n) ((n) >= 4 ? (560 + 2 * ((n)-4)) : (528 + 2 * (n)))
#define NUM_DBATnU(n) ((n) >= 4 ? (568 + 2 * ((n)-4)) : (536 + 2 * (n)))

#define GET_BAT(XBAT, n)                                                                 \
    ({                                                                                   \
        uint32_t _u, _l;                                                                 \
        asm volatile("mfspr %0, %1" : "=r"(_u) : "i"(NUM_##XBAT##nU(n)));                \
        asm volatile("mfspr %0, %1" : "=r"(_l) : "i"(NUM_##XBAT##nU(n) + 1));            \
        (struct bat){_u, _l};                                                            \
    })
#define SET_BAT(XBAT, n, val)                                                            \
    ({                                                                                   \
        struct bat _val = (val);                                                         \
        asm volatile("mtspr %0, %1" ::"i"(NUM_##XBAT##nU(n)), "r"(_val.u));              \
        asm volatile("mtspr %0, %1" ::"i"(NUM_##XBAT##nU(n) + 1), "r"(_val.l));          \
    })

#define DABR 1013

#define MY_SYSCALL_NUM 7

// kernel mode
static void
my_syscall_impl_memcpy(uint32_t dst, uint32_t src, uint32_t len) {
    struct bat old_dbat6 = GET_BAT(DBAT, 6);
    struct bat old_dbat7 = GET_BAT(DBAT, 7);

    struct bat new_dbat6 = {.u = 1 << 17 | 3, .l = (dst & 0xfffe0000) | 0b0010 << 3 | 2};
    struct bat new_dbat7 = {.u = 2 << 17 | 3, .l = (src & 0xfffe0000) | 0b0010 << 3 | 2};

    asm volatile("sync; isync");
    SET_BAT(DBAT, 6, new_dbat6);
    SET_BAT(DBAT, 7, new_dbat7);
    asm volatile("isync");

    char *dstp = (char *)(0x20000 | (dst & 0x1ffff));
    char *srcp = (char *)(0x40000 | (src & 0x1ffff));
    xmemcpy(dstp, srcp, len);
    flush_dc(dstp, len);

    asm volatile("sync; isync");
    SET_BAT(DBAT, 6, old_dbat6);
    SET_BAT(DBAT, 7, old_dbat7);
}

int
my_syscall_impl(uint32_t mode, uint32_t dst, uint32_t src, uint32_t len) {
    uint32_t old_dabr;
    asm volatile("mfspr %0, %1" : "=r"(old_dabr) : "i"(DABR));
    asm volatile("mtspr %0, %1" ::"i"(DABR), "r"(0));
    asm volatile("isync");

    int ret = -1;
    switch (mode) {
    case MSM_MEMCPY:
        my_syscall_impl_memcpy(dst, src, len);
        ret = 0;
        break;
    case MSM_INVAL_IC:
        inval_ic((void *)dst, len);
        ret = 0;
        break;
    case MSM_TEST:
        ret = 0;
        break;
    case MSM_ENABLE_IBAT0_KERN_EXEC: {
        struct bat bat = GET_BAT(IBAT, 0);
        bat.u |= 3;
        SET_BAT(IBAT, 0, bat);
        ret = 0;
        break;
    }
    default:
        break;
    }

    asm volatile("mtspr %0, %1" ::"i"(DABR), "r"(old_dabr));
    asm volatile("isync");

    return ret;
}

#define REG(r, val) register uint32_t r asm(#r) = (val)

int
my_syscall(enum my_syscall_mode mode, uint32_t dst, uint32_t src, uint32_t len) {
    REG(r0, MY_SYSCALL_NUM * 0x100);
    REG(r3, mode);
    REG(r4, dst);
    REG(r5, src);
    REG(r6, len);
    /*
    Warning: Do not modify the contents of input-only operands (except for inputs tied to
    outputs). The compiler assumes that on exit from the asm statement these operands
    contain the same values as they had before executing the statement. It is not possible
    to use clobbers to inform the compiler that the values in these inputs are changing.
    One common work-around is to tie the changing input variable to an output variable
    that never gets used.

    what the fuck.

    TODO remove this paste
    */

    asm volatile("mflr 29\n" // see hbl_kern_write
                 "sc\n"
                 "mtlr 29\n"
                 : "+r"(r0), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6)::"memory", "ctr",
                   "lr", "cc", "r7", "r8", "r9", "r10", "r11", "r12", "r29");
    return (int)r3;
}

static uint32_t
my_OSEffectiveToPhysical(const volatile void *addr) {
    // 0xfff00000 is identity mapped in IBATs, but doesn't show up in
    // OSEffectiveToPhysical and isn't mapped in DBATs
    uintptr_t paddr = (uintptr_t)addr;
    if ((paddr - 0xfff00000ul) < 0x20000) {
        return paddr;
    }
    return OSEffectiveToPhysical(addr);
}

#define LOG_MEMCPY 0
size_t
priv_try_memcpy(volatile void *dst, const volatile void *src, size_t len) {
    size_t remaining = len;
    volatile char *curdst = (char *)dst;
    const volatile char *cursrc = (char *)src;
    // ensure(src);
    // ensure(dst);
    while (remaining > 0) {
        uint32_t physdst = my_OSEffectiveToPhysical(curdst);
        uint32_t physsrc = my_OSEffectiveToPhysical(cursrc);
        if (LOG_MEMCPY || !physdst || !physsrc)
            log("priv_memcpy: curdst=%p;physdst=%x cursrc=%p;physsrc=%x remaining=%x\n",
                curdst, physdst, cursrc, physsrc, (int)remaining);
        if (!physdst || !physsrc)
            break;
        uint32_t xlen
            = min(min(0x1000u - (physdst & 0xfffu), 0x1000u - (physsrc & 0xfffu)),
                  (uint32_t)remaining);
        ensure(0 == my_syscall(MSM_MEMCPY, physdst, physsrc, xlen));
        curdst += xlen;
        cursrc += xlen;
        remaining -= xlen;
    }
    size_t copied = len - remaining;
    if ((uintptr_t)dst < 0xff000000) {
        // this REALLY should not be necessary.  so why is it? XXX this actually doesn't solve the problem
        flush_dc(dst, copied);
    }
    ensure(0 == my_syscall(MSM_INVAL_IC, (uint32_t)dst, 0, copied));
#if 0 // with gdbstub, racy reads/writes are possible
    if (!((uintptr_t)dst >= 0xfff00000 || (uintptr_t)src >= 0xfff00000))
        ensure(xmemcmp(dst, src, copied) == 0);
#endif
    return copied;
}
void
priv_memcpy(volatile void *dst, const volatile void *src, size_t len) {
    size_t copied = priv_try_memcpy(dst, src, len);
    if (copied != len)
        panic("priv_try_memcpy failed (%d/%d)\n", (int)copied, (int)len);
}

void
patch_security_level(void) {
    uint32_t insns[] = {0x38600001, 0x4e800020}; // li 3, 1; blr
    priv_memcpy((void *)OSGetSecurityLevel, insns, sizeof(insns));
}

void
patch_kernel_devmode(void) {
    // find kernel copy of platform info
    log("patch_kernel_devmode:\n");
    uint32_t search_start = 0xffeb0000;
    uint32_t search_end = 0xffed0000;
    uint32_t search_size = search_end - search_start;
    char *buf = (char *)MEMAllocFromDefaultHeap(search_size);
    ensure(buf);
    priv_memcpy(buf, (void *)search_start, search_size);
    uint32_t flags = __OSPlatformInfo.flags;
    uint32_t found_off = -1u;
    for (uint32_t off = 0; off <= search_size - sizeof(struct platform_info); off += 4) {
        struct platform_info *pi = (struct platform_info *)(buf + off);
        if (pi->flags == flags
            && !memcmp(pi, &__OSPlatformInfo, sizeof(struct platform_info))) {
            if (found_off != -1u)
                panic("patch_kernel_devmode: got multiple offsets for platform info");
            found_off = off;
        }
    }
    MEMFreeToDefaultHeap(buf);
    if (found_off == -1u)
        panic("patch_kernel_devmode: didn't find platform info");
    uint32_t new_flags = flags | 1 << 27;
    __OSPlatformInfo.flags = new_flags;
    priv_memcpy((char *)search_start + found_off + offsetof(struct platform_info, flags),
                &new_flags, sizeof(uint32_t));
}

void
patch_kernel_ibat0l_writes(void) {
    uint32_t search_start = 0xfff00000;
    uint32_t search_end = 0xfff1e000;
    uint32_t search_size = search_end - search_start;
    uint32_t *buf = (uint32_t *)MEMAllocFromDefaultHeap(search_size);
    ensure(buf);
    priv_memcpy(buf, (void *)search_start, search_size);
    for (uint32_t i = 0; i < search_size / 4; i++) {
        if ((buf[i] & ~0x3e00000u) == 0x7c1083a6) {
            uint32_t addr = search_start + 4 * i;
            log("found ibat0l write at %x, patching\n", addr);
            uint32_t val = 0x60000000;
            priv_memcpy((void *)addr, &val, 4);
        }
    }
    MEMFreeToDefaultHeap(buf);
}

void
patch_tracestub(void) {
    asm volatile(".pushsection .text.tracestub\n"
                 // Stupid stub to make trace exception look like DSI
                 "tracestub:\n"
                 "mtsprg2 22\n"
                 "li 22, 1\n"
                 "mtdsisr 22\n"
                 "mfsprg2 22\n"
                 "ba 0xfff00300\n"
                 // and ditto for instruction address break
                 "iabstub:\n"
                 "mtsprg2 22\n"
                 "li 22, 2\n"
                 "mtdsisr 22\n"
                 "mfsprg2 22\n"
                 "ba 0xfff00300\n"
                 ".popsection\n");
    extern char tracestub[];
    extern char iabstub[];
    // 0xfff00000 is identity mapped in IBATs, but doesn't show up in
    // OSEffectiveToPhysical and isn't mapped in DBATs
    size_t size = 5 * 4;
    void *tmp = MEMAllocFromDefaultHeapEx(size, 0x20); // needed?
    ensure(tmp);
    memcpy(tmp, tracestub, size);
    priv_memcpy((void *)0xfff00d00, tmp, size);
    memcpy(tmp, iabstub, size);
    priv_memcpy((void *)0xfff01300, tmp, size);
    MEMFreeToDefaultHeap(tmp);
}

void
dump_rpls(void) {
    patch_security_level();

    int count = OSDynLoad_GetNumberOfRPLs();
    log("# RPLs: %d\n", count);
    ensure(count >= 0);
    struct rpl_info *buf
        = (struct rpl_info *)MEMAllocFromDefaultHeap(sizeof(struct rpl_info) * (size_t)count);
    ensure(buf);
    ensure(0 != OSDynLoad_GetRPLInfo(0, count, buf));
    for (int i = 0; i < count; i++) {
        struct rpl_info *info = &buf[i];
        log("rpls[%d]: name=%s, text{%08x..%08x slide %#x} data{%08x..%08x slide %#x} "
            "rodata{%08x..%08x slide %#x}\n",
            i, info->name,
            info->text_addr, info->text_addr + info->text_size,
            info->text_slide, info->data_addr, info->data_addr + info->data_size,
            info->data_slide, info->rodata_addr, info->rodata_addr + info->rodata_size,
            info->rodata_slide);
    }
    MEMFreeToDefaultHeap(buf);
}

static bool
str_endswith(const char *str, const char *suffix) {
    size_t str_len = strlen(str);
    size_t suffix_len = strlen(suffix);
    return str_len >= suffix_len
        && !memcmp(str + (str_len - suffix_len), suffix, suffix_len);
}

bool
find_rpl_info(struct rpl_info *out, const char *name_suffix) {
    patch_security_level();
    int count = OSDynLoad_GetNumberOfRPLs();
    ensure(count >= 0);
    struct rpl_info *buf
        = (struct rpl_info *)MEMAllocFromDefaultHeap(sizeof(struct rpl_info) * (size_t)count);
    ensure(buf);
    ensure(0 != OSDynLoad_GetRPLInfo(0, count, buf));
    bool ret = false;
    for (int i = 0; i < count; i++) {
        struct rpl_info *info = &buf[i];
        if (str_endswith(info->name, name_suffix)) {
            *out = *info;
            ret = true;
            break;
        }
    }
    MEMFreeToDefaultHeap(buf);
    return ret;
}

static void
hbl_kern_write(void *addr, uint32_t val) {
    REG(r0, 0x3500);
    REG(r3, 1);
    REG(r4, 0);
    REG(r5, (uint32_t)val);
    REG(r6, 0);
    REG(r7, 0);
    REG(r8, 0x10000);
    REG(r9, (uint32_t)addr);
    asm volatile("mr 28, 1\n"
                 "mflr 29\n" // because clang is broken
                 "sc\n"
                 "mtlr 29\n"
                 "mr 1, 28\n"
                 : "+r"(r0), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7), "+r"(r8),
                   "+r"(r9)::"memory", "ctr", "lr", "cc", "r10", "r11", "r12", "r28",
                   "r29");
}

static uint32_t
hbl_kern_read(const void *addr) {
    REG(r0, 0x3400);
    REG(r3, 1);
    REG(r4, 0);
    REG(r5, 0);
    REG(r6, 0);
    REG(r7, 0);
    REG(r8, 0x10000);
    REG(r9, (uint32_t)addr);
    asm volatile("mr 28, 1\n"
                 "mflr 29\n" // because clang is broken
                 "sc\n"
                 "mtlr 29\n"
                 "mr 1, 28\n"
                 : "+r"(r0), "+r"(r3), "+r"(r4), "+r"(r5), "+r"(r6), "+r"(r7), "+r"(r8),
                   "+r"(r9)::"memory", "ctr", "lr", "cc", "r10", "r11", "r12", "r28",
                   "r29");
    return r3;
}

void
install_syscall(void *the_syscall_impl) {
    log("install_syscall(%p)\n", the_syscall_impl);
    for (int i = 0; i < 5; i++) {
        void *base = OS_SPECIFICS->kern_syscall_tbl[i];
        void *addr = (char *)base + MY_SYSCALL_NUM * 4;
        // log("%d: base=%p addr=%p old=%08x\n", i, base, addr, hbl_kern_read(addr));
        hbl_kern_write(addr, (uint32_t)the_syscall_impl);
        ensure(hbl_kern_read(addr) == (uint32_t)the_syscall_impl);
    }
}

static void
enable_0100xxxx_kern_exec_cb(int core, void *event) {
    // run the kern_exec_stub
    OSMemoryBarrier();
    log("enable_0100xxxx_kern_exec_cb: core=%d\n", core);
    my_syscall(MSM_ENABLE_IBAT0_KERN_EXEC, 0, 0, 0);
    OSSignalEvent((OSEvent *)event);
}

void
enable_0100xxxx_kern_exec(void) {
    OSEvent events[3];
    OSMemoryBarrier();
    for (int core = 0; core <= 2; core++) {
        OSInitEvent(&events[core], false, false);
        OSThread *thread = OSGetDefaultThread(core);
        if (OSGetCurrentThread() == thread)
            enable_0100xxxx_kern_exec_cb(core, &events[core]);
        else
            ensure(
                OSRunThread(thread, (void *)enable_0100xxxx_kern_exec_cb, core, &events[core]));
    }
    for (int core = 0; core <= 2; core++)
        OSWaitEvent(&events[core]);
    log("enable_0100xxxx_kern_exec: done\n");
}
