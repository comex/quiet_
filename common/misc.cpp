#include "misc.h"
#include "decls.h"
#include "loader.h"
#include "logging.h"
#include "kern_garbage.h"

#if !DUMMY
void *
memchr(const void *_s, int c, size_t n) {
    const uint8_t *s = (const uint8_t *)_s;
    while (n--) {
        if (*s == c)
            return (void *)s;
        s++;
    }
    return nullptr;
}

size_t
strlen(const char *s) {
    size_t ret = 0;
    while (*s++)
        ret++;
    return ret;
}

size_t
strnlen(const char *s, size_t max) {
    size_t ret = 0;
    while (max-- && *s++)
        ret++;
    return ret;
}

char *
stpcpy(char *s, const char *src) {
    while ((*s++ = *src++))
        ;
    return s - 1;
}

int
strncmp(const char *s1, const char *s2, size_t n) {
    while (n--) {
        char c1 = *s1++, c2 = *s2++;
        if (c1 != c2)
            return c1 - c2;
        else if (!c1)
            return 0;
    }
    return 0;
}

int
strcmp(const char *s1, const char *s2) {
    return strncmp(s1, s2, (size_t)-1);
}
#endif

// clang-format off
#define LOL_UNROLL_i(code, _i) \
    { enum { i = (_i) }; code; }
#define LOL_UNROLL_8(code) \
    LOL_UNROLL_i(code, 0) LOL_UNROLL_i(code, 1) LOL_UNROLL_i(code, 2) \
    LOL_UNROLL_i(code, 3) LOL_UNROLL_i(code, 4) LOL_UNROLL_i(code, 5) \
    LOL_UNROLL_i(code, 6) LOL_UNROLL_i(code, 7)
// clang-format on

void
xmemcpy(volatile void *a, const volatile void *b, size_t size) {
    while (size > 0) {
        if (((uintptr_t)a & (uintptr_t)b & 3) == 0 && size >= 0x20) {
            do {
                uint32_t tmp[8];
                LOL_UNROLL_8(tmp[i] = ((volatile uint32_t *)b)[i]);
                LOL_UNROLL_8(((volatile uint32_t *)a)[i] = tmp[i]);
                a = (char *)a + 0x20;
                b = (char *)b + 0x20;
                size -= 0x20;
            } while (size >= 0x20);
        } else {
            *(volatile uint8_t *)a = *(volatile uint8_t *)b;
            a = (char *)a + 1;
            b = (char *)b + 1;
            size--;
        }
    }
}

void
xbzero(volatile void *a, size_t size) {
    while (size > 0) {
        if (((uintptr_t)a & 3) == 0 && size >= 0x20) {
            do {
                LOL_UNROLL_8(((volatile uint32_t *)a)[i] = 0);
                a = (char *)a + 0x20;
                size -= 0x20;
            } while (size >= 0x20);
        } else {
            *(volatile uint8_t *)a = 0;
            a = (char *)a + 1;
            size--;
        }
    }
}

int
xmemcmp(const volatile void *a, const volatile void *b, size_t size) {
    const volatile char *pa = (char *)a, *pb = (char *)b;
    while (size--) {
        char ca = *pa++, cb = *pb++;
        if (ca != cb)
            return ca - cb;
    }
    return 0;
}

#if !DUMMY
void
inval_dc(volatile void *dst, size_t size) {
    ssize_t remaining = (ssize_t)size;
    while (remaining > 0) {
        asm volatile("dcbi 0, %0" ::"b"(dst));
        dst = (char *)dst + 0x20;
        remaining -= 0x20;
    }
    asm volatile("sync; eieio");
}

void
flush_dc(const volatile void *dst, size_t size) {
    ssize_t remaining = (ssize_t)size;
    while (remaining > 0) {
        asm volatile("dcbf 0, %0" ::"b"(dst));
        dst = (char *)dst + 0x20;
        remaining -= 0x20;
    }
    asm volatile("sync; eieio");
}

void
inval_ic(volatile void *dst, size_t size) {
    ssize_t remaining = (ssize_t)size;
    while (remaining > 0) {
        asm volatile("icbi 0, %0" ::"b"(dst));
        dst = (char *)dst + 0x20;
        remaining -= 0x20;
    }
    asm volatile("sync; eieio; isync; nop; nop; nop; nop; nop; nop; nop; nop");
}
#endif

uint32_t
adjust_branch(uint32_t orig, uint32_t src, uint32_t dst) {
    uint32_t diff = dst - src;
    uint32_t masked = diff & 0x03fffffcu;
    uint32_t extended = masked | ((masked & 0x02000000) ? 0xfc000000 : 0);
    if (!DUMMY && diff != extended)
        panic("out-of-range branch from 0x%08x to 0x%08x", src, dst);
    return (orig & ~0x03fffffcu) | masked;
}

uint32_t
get_branch(uint32_t src, uint32_t dst) {
    return adjust_branch(0x48000000, src, dst);
}

uint32_t
get_ba(uint32_t dst) {
    uint32_t masked = dst & 0x03fffffcu;
    uint32_t extended = masked | ((masked & 0x02000000) ? 0xfc000000 : 0);
    if (!DUMMY && dst != extended)
        panic("out-of-range ba to 0x%08x", dst);
    return 0x48000002 | masked;
}

UNUSED static void
get_indirect_jump(uint32_t insns[4], uint32_t dst) {
    uint32_t reg = 13;
    insns[0] = 0x3c000000 | (reg << 21) | (dst >> 16); // lis reg, ...
    insns[1] = 0x60000000 | (reg << 21) | (reg << 16) | (dst & 0xffff); // ori reg, reg, ...
    insns[2] = 0x7c0903a6 | (reg << 21); // mtctr reg
    insns[3] = 0x4e800420; // bctr
}

UNUSED static inline bool
identify_branch(uint32_t insn, uint32_t src, uint32_t *dst_p, bool *is_ba_p, bool *is_bl_p) {
    if ((insn & 0xfc000000u) == 0x48000000u) {
        uint32_t masked = insn & 0x03fffffcu;
        uint32_t extended = masked | ((masked & 0x02000000) ? 0xfc000000 : 0);
        *is_ba_p = !!(insn & 2);
        *is_bl_p = !!(insn & 1);
        *dst_p = *is_ba_p ? extended : ((uint32_t)src + extended);
        return true;
    } else {
        return false;
    }
}

#if !DUMMY
#define HOOK_MAGIC ((uint32_t)'HOOK')
void
install_hook(const char *foo_name, void *foo, void *hook_foo, void *orig_foo) {
    _log(LOG_FLAGS_TO_USE | LOG_COS, "  install_hook(%s): fptr=%p hook=%p orig=%p\n", foo_name, foo, hook_foo, orig_foo);
    uint32_t first_insn = *(uint32_t *)foo;
    uint32_t first_insn_fixed = first_insn;
    uint32_t dst;
    bool is_ba, is_bl;
    if (identify_branch(first_insn, (uint32_t)foo, &dst, &is_ba, &is_bl) && !is_ba) {
        // fix it up
        first_insn_fixed = adjust_branch(first_insn, (uint32_t)orig_foo, dst);
    }
    uint32_t code_for_orig_foo[7];
    code_for_orig_foo[0] = first_insn_fixed;
    get_indirect_jump(&code_for_orig_foo[1], (uint32_t)foo + 4);
    code_for_orig_foo[5] = HOOK_MAGIC;
    code_for_orig_foo[6] = first_insn;
    //log("    first_insn=%08x\n", first_insn);
    priv_memcpy(orig_foo, code_for_orig_foo, sizeof(code_for_orig_foo));
    // this is liive
    uint32_t code_for_foo = get_ba((uint32_t)hook_foo);
    priv_memcpy(foo, &code_for_foo, sizeof(code_for_foo));
}

void
uninstall_hook(const char *foo_name, void *foo, void *hook_foo, void *orig_foo) {
    _log(LOG_FLAGS_TO_USE | LOG_COS, "  uninstall_hook(%s): fptr=%p hook=%p orig=%p\n", foo_name, foo, hook_foo, orig_foo);
    const uint32_t *code_for_orig_foo = (uint32_t *)orig_foo;
    ensure_eq(code_for_orig_foo[5], HOOK_MAGIC);
    uint32_t first_insn = code_for_orig_foo[6];
    uint32_t *ptr = (uint32_t *)foo;
    while (1) {
        uint32_t insn = *ptr, dst;
        bool is_ba, is_bl;
        if (!identify_branch(insn, (uint32_t)ptr, &dst, &is_ba, &is_bl) || is_bl)
            panic("uninstall_hook: unexpected first insn %08x", insn);
        if (dst == (uint32_t)hook_foo)
            break;
        ptr = (uint32_t *)dst;
    }

    priv_memcpy(ptr, &first_insn, 4);
    uint32_t zeroes[7] = {0};
    priv_memcpy(orig_foo, zeroes, sizeof(zeroes));
}

void
install_hooks(const struct func_hook_info *list) {
    for (size_t i = 0; ; i++) {
        const struct func_hook_info *fhi = &list[i];
        if (!(fhi->flags & HOOK_VALID))
            break;
        void *fptr = fhi->fptr_or_ptrptr;
        if (fhi->flags & HOOK_FPTR_IS_PTRPTR)
            fptr = *(void **)fptr;
        install_hook(fhi->name, fptr, fhi->hook, fhi->orig);
    }

}
void
uninstall_hooks(const struct func_hook_info *list) {
    for (size_t i = 0; ; i++) {
        const struct func_hook_info *fhi = &list[i];
        if (!(fhi->flags & HOOK_VALID))
            break;
        void *fptr = fhi->fptr_or_ptrptr;
        if (fhi->flags & HOOK_FPTR_IS_PTRPTR)
            fptr = *(void **)fptr;
        uninstall_hook(fhi->name, fptr, fhi->hook, fhi->orig);
    }
}
#endif

#if !DUMMY
int
usleep(uint32_t us) {
    struct system_info *si = OSGetSystemInfo();
    OSSleepTicks(((uint64_t)us * si->clock + 3999999) / 4000000);
    return 0;
}
#endif

uint64_t
cur_time_us(void) {
#if DUMMY
    struct timespec ret;
    ensure(!clock_gettime(CLOCK_MONOTONIC, &ret));
    return (uint64_t)ret.tv_sec * 1000000 + ((uint64_t)ret.tv_nsec / 1000);
#else
    if (!g_self_fixup_done)
        return 0;
    struct system_info *si = OSGetSystemInfo();
    uint32_t div = si->clock / 4000000;
    return (uint64_t)OSGetTime() / div;
#endif
}

#undef memcmp
int
memcmp(const void *_s1, const void *_s2, size_t n) {
    const uint8_t *s1 = (uint8_t *)_s1, *s2 = (uint8_t *)_s2;
    while (n--) {
        int diff = (int)*s1++ - (int)*s2++;
        if (diff != 0)
            return diff;
    }
    return 0;
}

void
dump_my_ips(void) {
#if !DUMMY
    int ret;
    if ((ret = netconf_init())) {
        log("netconf_init() -> %d\n", ret);
    } else {
        for (int interface = 0; interface < 2; interface++) {
            struct in_addr myaddr;
            if ((ret = netconf_get_assigned_address(interface, &myaddr))) {
                log("[%d] netconf_get_assigned_address() -> %d\n", interface, ret);
            } else if (myaddr.s_addr != 0) {
                char *buf = inet_ntoa(myaddr);
                ensure(buf);
                log("[%d] IP: %s\n", interface, buf);
            }
        }
    }
#endif
}

void
socket_lib_init_wrapper(void) {
#if !DUMMY
    int ret;
    if ((ret = socket_lib_init()))
        panic("socket_lib_init(): %d [%d]", ret, socketlasterr());
#endif
}

#if DUMMY
thread_local OSThread *cur_osthread;
#endif
