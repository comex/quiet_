#include "loader.h"
#include "decls.h"
#include "elf.h"
#include "misc.h"
#include "types.h"
#include "usbducks.h"
#include "kern_garbage.h"

#if BONUS_FEATURES
#include "loud.h"
#endif

BEGIN_LOCAL_DECLS

#if !DUMMY

extern const struct func_hook_info OSBlockThreadsOnExit_hook[];

constexpr uint32_t LOADER_COMM_MAGIC = (uint32_t)'LCOM';
struct loader_comm {
    uint32_t magic;
    const struct func_hook_info *OSBlockThreadsOnExit_hook;
};

__attribute__((section(".notrodata"))) typeof(OSFatal) *OSFatal_ptr = nullptr;

static void
early_init(void) {
    void *lib;
    if (OS_SPECIFICS->p_OSDynLoad_Acquire("coreinit.rpl", &lib) || //
        OS_SPECIFICS->p_OSDynLoad_FindExport(lib, false, "OSFatal", (void **)&OSFatal_ptr)) {
        while (1)
            __builtin_trap();
    }
}

static void
make_hbl_not_reload_me(void) {
    // this is awful that we have to do this ourselves
    MAIN_ENTRY_ADDR = 0xC001C0DE;
}

#endif

#include "logging.h"

struct tinyheap_slot {
    void *ptr;
    int size;
    int prev_slot_idx;
    int next_slot_idx;
};

struct tinyheap {
    void *start;
    void *end;
    int first_slot_idx;
    int last_slot_idx;
    int first_free_slot_idx;
    struct tinyheap_slot slots[];
};

struct loader_shared_block {
    void *uncompressed;
    void *compressed;
    uint32_t compressed_len; // 0 if uncompressed
    uint32_t uncompressed_len;
};

struct loader_shared {
    struct rpldata *first_rpldata;
    void *data_base;
    uint32_t copy_block_count;
    struct loader_shared_block *copy_blocks;
    uint32_t zero_block_count;
    struct loader_shared_block *zero_blocks;
};

struct rpldata {
    char skip0[0x4a - 0];
    uint16_t shentsz;
    uint16_t shcount;
    Elf32_Shdr *shdrs;
    char skip54[0xf4 - 0x54];
    void **section_datas;
    char skipf8[0x114 - 0xf8];
    struct rpldata *next_rpldata;
};
#if !DUMMY
static_assert(sizeof(struct rpldata) == 0x118, "rpldata");
#endif

#define g_loader_shared ((struct loader_shared *)0xFA000000)
#define g_shared_code_heap_tracking ((struct tinyheap *)0xFA000018)
#define g_shared_read_heap_tracking ((struct tinyheap *)0xFA000848)

#if !DUMMY
static void
priv_bzero(volatile void *dst, size_t len) {
    char tmp[256] __attribute__((aligned(0x100)));
    xbzero(tmp, sizeof(tmp));
    while (len > 0) {
        size_t xlen = min(len, sizeof(tmp));
        priv_memcpy(dst, tmp, xlen);
        len -= xlen;
        dst = (char *)dst + xlen;
    }
}
static void *
tinyheap_alloc(struct tinyheap *theap, size_t size, size_t align) {
    size_t padded4_size = (size + 3ul) & ~3ul;
    if (padded4_size < size)
        return nullptr;
    for (int slot_idx = theap->first_slot_idx; slot_idx != -1; slot_idx = theap->slots[slot_idx].next_slot_idx) {
        struct tinyheap_slot *slot = &theap->slots[slot_idx];
        if (slot->size >= 0)
            continue; // in use
        void *ptr = slot->ptr;
        size_t avail = (size_t)-slot->size;
        size_t needed_to_align = -(uintptr_t)ptr & (align - 1);
        size_t padded_size = padded4_size + needed_to_align;
        if (padded_size < padded4_size
            || padded_size > avail) // xxx, theoretically could work if == avail
            continue;
        // OK, this block works
        struct tinyheap_slot slot_tmp = *slot;
        int next_slot_idx = slot_tmp.next_slot_idx;
        int new_slot_idx = theap->first_free_slot_idx;
        if (new_slot_idx == -1)
            return nullptr;
        struct tinyheap_slot *new_slot = &theap->slots[new_slot_idx];
        int new_first_free_slot_idx = new_slot->next_slot_idx;
        struct tinyheap_slot new_slot_tmp = {
            .ptr = ptr,
            .size = (int)padded_size,
            .prev_slot_idx = slot_idx,
            .next_slot_idx = next_slot_idx,
        };
        slot_tmp.ptr = (char *)slot_tmp.ptr + padded_size;
        slot_tmp.size += (int)padded_size; // less negative
        slot_tmp.next_slot_idx = new_slot_idx;

        struct tinyheap theap_tmp = *theap;
        theap_tmp.first_free_slot_idx = new_first_free_slot_idx;
        if (new_first_free_slot_idx != -1) {
            struct tinyheap_slot *new_first_free_slot = &theap->slots[new_first_free_slot_idx];
            ensure_eq(new_first_free_slot->prev_slot_idx, new_slot_idx);
            int val = -1;
            priv_memcpy(&new_first_free_slot->prev_slot_idx, &val, sizeof(val));
        }
        if (next_slot_idx != -1) {
            struct tinyheap_slot *next_slot = &theap->slots[next_slot_idx];
            ensure_eq(next_slot->prev_slot_idx, slot_idx);
            priv_memcpy(&next_slot->prev_slot_idx, &new_slot_idx, sizeof(new_slot_idx));
        } else {
            theap_tmp.last_slot_idx = new_slot_idx;
        }

        priv_memcpy(new_slot, &new_slot_tmp, sizeof(struct tinyheap_slot));
        priv_memcpy(slot, &slot_tmp, sizeof(struct tinyheap_slot));
        priv_memcpy(theap, &theap_tmp, sizeof(struct tinyheap));
        return (char *)ptr + needed_to_align;
    }
    return nullptr; // not enough space
}

static void
tinyheap_unlink_block(struct tinyheap *theap, int idx) {
    struct tinyheap_slot *slot = &theap->slots[idx];
    int prev_idx = slot->prev_slot_idx, next_idx = slot->next_slot_idx;
    if (prev_idx != -1) {
        struct tinyheap_slot *prev_slot = &theap->slots[prev_idx];
        ensure(prev_slot->next_slot_idx == idx);
        priv_memcpy(&prev_slot->next_slot_idx, &next_idx, sizeof(int));
    } else {
        ensure(theap->first_slot_idx == idx);
        priv_memcpy(&theap->first_slot_idx, &next_idx, sizeof(int));
    }
    if (next_idx != -1) {
        struct tinyheap_slot *next_slot = &theap->slots[next_idx];
        ensure(next_slot->prev_slot_idx == idx);
        priv_memcpy(&next_slot->prev_slot_idx, &prev_idx, sizeof(int));
    } else {
        ensure(theap->last_slot_idx == idx);
        priv_memcpy(&theap->last_slot_idx, &prev_idx, sizeof(int));
    }

    priv_memcpy(&slot->next_slot_idx, &prev_idx, sizeof(int));
    struct tinyheap_slot slot_tmp = {
        .ptr = nullptr, .size = 0,
        .prev_slot_idx = -1, .next_slot_idx = theap->first_free_slot_idx
    };
    if (theap->first_free_slot_idx != -1) {
        struct tinyheap_slot *first_free_slot = &theap->slots[theap->first_free_slot_idx];
        priv_memcpy(&first_free_slot->prev_slot_idx, &idx, sizeof(int));
    }
    priv_memcpy(&theap->first_free_slot_idx, &idx, sizeof(int));
    priv_memcpy(slot, &slot_tmp, sizeof(slot_tmp));
}

UNUSED static void
tinyheap_free(struct tinyheap *theap, void *ptr) {
    int idx = -1;
    for (int tmp_idx = theap->first_slot_idx; tmp_idx != -1; tmp_idx = theap->slots[tmp_idx].next_slot_idx) {
        struct tinyheap_slot *slot = &theap->slots[idx];
        void *slot_ptr = slot->ptr;
        if (ptr >= slot_ptr && ptr < (char *)slot_ptr + slot->size) {
            ensure(idx == -1);
            idx = tmp_idx;
        }
    }
    ensure(idx != -1);
    struct tinyheap_slot *slot = &theap->slots[idx];
    ensure(slot->size >= 0); // in use
    int size = slot->size;
    if (slot->prev_slot_idx != -1) {
        int prev_idx = slot->prev_slot_idx;
        struct tinyheap_slot *prev_slot = &theap->slots[prev_idx];
        if (prev_slot->size < 0) {
            // Merge prev_slot | slot
            ensure(slot->ptr == (char *)prev_slot->ptr + (-prev_slot->size));
            size = -prev_slot->size + size;
            tinyheap_unlink_block(theap, idx);
            idx = prev_idx;
            slot = prev_slot;
        }
    }

    if (slot->next_slot_idx != -1) {
        int next_idx = slot->next_slot_idx;
        struct tinyheap_slot *next_slot = &theap->slots[next_idx];
        if (next_slot->size < 0) {
            // Merge slot | next_slot
            ensure(next_slot->ptr == (char *)slot->ptr + size);
            size += (-next_slot->size);
            tinyheap_unlink_block(theap, next_idx);
        }
    }

    // Mark as free
    size = -size;
    priv_memcpy(&slot->size, &size, sizeof(int));
}

static void
dump_blocks(uint32_t count, struct loader_shared_block *blocks) {
    log("  count=%u blocks=%p..%p\n", count, blocks, blocks + count);
    for (uint32_t i = 0; i < count; i++)
        log("    [%d] = %p, %p, 0x%x, 0x%x\n", i, blocks[i].uncompressed,
            blocks[i].compressed, blocks[i].compressed_len, blocks[i].uncompressed_len);
}

static void
add_shared_block(uint32_t *countp, struct loader_shared_block **blocksp,
                 struct loader_shared_block new_block) {
    uint32_t count = *countp, new_count = count + 1;
    struct loader_shared_block *blocks = *blocksp;
    log("new_count=%u\n", count);
    struct loader_shared_block *new_blocks = (struct loader_shared_block *)tinyheap_alloc(
        g_shared_read_heap_tracking, new_count * sizeof(struct loader_shared_block), 4);
    ensure(new_blocks);
    log("new_blocks=%p\n", new_blocks);
    priv_memcpy(new_blocks, blocks, count * sizeof(struct loader_shared_block));
    priv_memcpy(&new_blocks[count], &new_block, sizeof(struct loader_shared_block));
    priv_memcpy(blocksp, &new_blocks, sizeof(struct loader_shared_block *));
    priv_memcpy(countp, &new_count, sizeof(uint32_t));
}

static void
dump_theap_from(struct tinyheap *theap, int start_idx) {
    for (int idx = start_idx, prev_idx = -1;
         idx != -1;
         prev_idx = idx, idx = theap->slots[idx].next_slot_idx) {
        struct tinyheap_slot *slot = &theap->slots[idx];
        log("    %d: %p size=%d prev=%d next=%d\n", idx, slot->ptr, slot->size,
            slot->prev_slot_idx, slot->next_slot_idx);
        if (slot->prev_slot_idx != prev_idx)
            log("    ** expected prev=%d!\n", prev_idx);
        /*
        uint32_t *p = slot->ptr;
        log("      [0x%08x 0x%08x]\n",
            p[0], p[1]);
        */
    }
}
static void
dump_theap(struct tinyheap *theap) {
    log("  start=%p end=%p first_slot_idx=%d last_slot_idx=%d first_free_slot_idx=%d\n", theap->start,
        theap->end, theap->first_slot_idx, theap->last_slot_idx, theap->first_free_slot_idx);
    log("  slots:\n");
    dump_theap_from(theap, theap->first_slot_idx);
    log("  free slots:\n");
    dump_theap_from(theap, theap->first_free_slot_idx);
}

UNUSED static void
dump_loader_info(void) {
    struct loader_shared *ls = g_loader_shared;
    log("first_rpldata:%p data_base:%p\n", ls->first_rpldata, ls->data_base);
    for (int i = 0; i < 3; i++) {
        void *addr = (void *)0xeeeeeeee;
        size_t size = 0xeeeeeeee;
        int ret = OSGetMemBound(i, &addr, &size);
        log("MemBound[%d]: ret=%d %p 0x%x\n", i, ret, addr, (int)size);
    }
    {
        void *addr = (void *)0xeeeeeeee;
        size_t size = 0xeeeeeeee;
        int ret = OSGetForegroundBucket(&addr, &size);
        log("ForegroundBucket: ret=%d %p 0x%x\n", ret, addr, (int)size);
    }

    log("copy blocks (+1):\n");
    dump_blocks(ls->copy_block_count + 1, ls->copy_blocks);
    log("zero blocks:\n");
    dump_blocks(ls->zero_block_count, ls->zero_blocks);
    log("code heap:\n");
    dump_theap(g_shared_code_heap_tracking);
    log("read heap:\n");
    dump_theap(g_shared_read_heap_tracking);
}

#endif

static const char *
get_dynstr(size_t name_off, const char *strtab, size_t strtab_maxsize) {
    ensure(name_off < strtab_maxsize);
    size_t j;
    for (j = name_off; j < strtab_maxsize; j++) {
        if (strtab[j] == 0)
            break;
    }
    ensure(j < strtab_maxsize);
    return strtab + name_off;
}

#ifndef LOAD_ELF_VERBOSITY
#define LOAD_ELF_VERBOSITY 1
#endif

enum {
    LOAD_ELF_DO_LOAD = 1,
    LOAD_ELF_USE_PRIV = 2,
    LOAD_ELF_DO_LOCAL_RELOCS = 4,
    LOAD_ELF_DO_EXTERNAL_RELOCS = 8,
    LOAD_ELF_INPUT_USE_CHUNKWISE_IN_BUF = 0x10, // XXX
};

enum { LEI_NUM_CHUNKS = 2 };

struct load_elf_info_chunk {
    uint32_t sourceaddr;
    uint32_t destaddr;
    uint32_t filesz;
    uint32_t memsz;
    uint32_t offset;
    uint32_t align;
    uint32_t misalignment;
    void *staging;
    void *in_buf;
};
struct load_elf_info {
    void *in_buf;
    size_t in_bufsize;
    const Elf32_Ehdr *ehdr;
    int flags;
    struct load_elf_info_chunk chunks[LEI_NUM_CHUNKS];
    size_t dynamic_off;
    uint32_t loader_comm_addr_unslid;
    const Elf32_Phdr *phdrs;
    size_t phnum;
#define MAX_VNA_OTHER 16
    void *vna_other_to_lib[MAX_VNA_OTHER];
    size_t strtab_off;
    size_t symtab_off;
    size_t versym_off;
    size_t rela_off;
    size_t rela_size;
    size_t verneed_off;
    size_t verneed_count;
    uint32_t plt_addr_unslid;
    uint32_t entry_unslid;
};

#if DUMMY
static void
memcpy_code(volatile void *dst, const volatile void *src, size_t size,
            struct load_elf_info *in) {
    memcpy((void *)dst, (void *)src, size);
}
static void
bzero_code(volatile void *dst, size_t size, struct load_elf_info *in) {
    memset((void *)dst, 0, size);
}
#else
static void
memcpy_code(volatile void *dst, const volatile void *src, size_t size,
            struct load_elf_info *in) {
    if (in->flags & LOAD_ELF_USE_PRIV) {
        priv_memcpy(dst, src, size);
    } else {
        xmemcpy(dst, src, size);
        flush_dc(dst, size);
        inval_ic(dst, size);
    }
}
static void
bzero_code(volatile void *dst, size_t size, struct load_elf_info *in) {
    if (in->flags & LOAD_ELF_USE_PRIV) {
        priv_bzero(dst, size);
    } else {
        xbzero(dst, size);
        flush_dc(dst, size);
        inval_ic(dst, size);
    }
}
#endif

static uint32_t
addr_to_off(const struct load_elf_info *in, uint32_t addr, size_t size) {
    for (size_t i = 0; i < in->phnum; i++) {
        const Elf32_Phdr *ph = &in->phdrs[i];
        uint32_t poff = addr - bswap(ph->p_vaddr);
        uint32_t filesz = bswap(ph->p_filesz);
        if (poff < filesz && filesz - poff >= size)
            return poff + bswap(ph->p_offset);
    }
    panic("addr_to_off: %x", addr);
}

static void *
get_max_elf_data_by_offset(const struct load_elf_info *in, size_t offset,
                           size_t *maxsizep) {
    if (!(in->flags & LOAD_ELF_INPUT_USE_CHUNKWISE_IN_BUF)) {
        ensure_op(offset, <, in->in_bufsize);
        *maxsizep = in->in_bufsize - offset;
        return (char *)in->in_buf + offset;
    } else {
        for (size_t i = 0; i < LEI_NUM_CHUNKS; i++) {
            const struct load_elf_info_chunk *chunk = &in->chunks[i];
            size_t chunk_off = offset - chunk->offset;
            if (chunk_off < chunk->filesz) {
                *maxsizep = chunk->filesz - chunk_off;
                return (char *)chunk->in_buf + chunk_off;
            }
        }
        panic("get_max_elf_data_by_offset(0x%x)", (int)offset);
    }
}

static void *
get_elf_data_by_offset(const struct load_elf_info *in, size_t offset, size_t size) {
    size_t maxsize;
    void *ret = get_max_elf_data_by_offset(in, offset, &maxsize);
    ensure_op(maxsize, >=, size);
    return ret;
}

static uint32_t
get_final_load_address(const struct load_elf_info *in, uint32_t addr) {
    uint32_t ret = 0;
    int ok = 0;
    for (size_t i = 0; i < LEI_NUM_CHUNKS; i++) {
        const struct load_elf_info_chunk *chunk = &in->chunks[i];
        size_t chunk_off = addr - chunk->sourceaddr;
        if (chunk_off <= chunk->memsz) {
            ok++;
            ret = (uint32_t)(chunk->destaddr + chunk_off);
        }
    }
    if (ok != 1)
        panic("get_final_load_address(0x%x): %s", addr,
              ok ? "ambiguous" : "out of range");
    return ret;
}

static void *
get_staging_address(const struct load_elf_info *in, uint32_t addr, size_t size) {
    for (size_t i = 0; i < LEI_NUM_CHUNKS; i++) {
        const struct load_elf_info_chunk *chunk = &in->chunks[i];
        uint32_t chunk_off = addr - chunk->sourceaddr;
        uint32_t staging_size
            = (in->flags & LOAD_ELF_DO_LOAD) ? chunk->memsz : chunk->filesz;
        if (chunk_off < staging_size) {
            if (staging_size - chunk_off < size)
                break;
            return (char *)chunk->staging + chunk_off;
        }
    }
    panic("get_staging_address(0x%x, 0x%x)", addr, (int)size);
}

static void
load_elf_header(struct load_elf_info *restrict in) {
    const Elf32_Ehdr *eh = (Elf32_Ehdr *)get_elf_data_by_offset(in, 0, sizeof(*eh));
    ensure(eh->e_ident[EI_MAG0] == ELFMAG0 && eh->e_ident[EI_MAG1] == ELFMAG1
           && eh->e_ident[EI_MAG2] == ELFMAG2 && eh->e_ident[EI_MAG3] == ELFMAG3);
    ensure(eh->e_ident[EI_CLASS] == ELFCLASS32);
    ensure(bswap(eh->e_phentsize) == sizeof(Elf32_Phdr));
    in->entry_unslid = bswap(eh->e_entry);
    in->ehdr = eh;
}

static void
load_elf_phdrs(struct load_elf_info *restrict in) {
    const Elf32_Ehdr *eh = in->ehdr;
    size_t phnum = bswap(eh->e_phnum);
    const Elf32_Phdr *phdrs
        = (Elf32_Phdr *)get_elf_data_by_offset(in, bswap(eh->e_phoff), phnum * sizeof(*phdrs));
    in->phdrs = phdrs;
    in->phnum = phnum;
    in->dynamic_off = -1ul;
    ensure(phnum == 4);
    for (size_t i = 0; i < phnum; i++) {
        const Elf32_Phdr *ph = &phdrs[i];
        uint32_t vaddr = bswap(ph->p_vaddr), offset = bswap(ph->p_offset);
        uint32_t memsz = bswap(ph->p_memsz), filesz = bswap(ph->p_filesz);
        uint32_t type = bswap(ph->p_type), align = bswap(ph->p_align);
        ensure(memsz >= filesz);
        switch (i) {
        case 0 ... 1: {
            ensure(type == PT_LOAD);
            struct load_elf_info_chunk *chunk = &in->chunks[i];
            chunk->sourceaddr = vaddr;
            chunk->memsz = memsz;
            chunk->filesz = filesz;
            ensure(align);
            chunk->align = align;

            // important!  it's really the slide that needs to be aligned!
            chunk->misalignment = vaddr & (align - 1);

            chunk->offset = offset;
            chunk->filesz = min(chunk->filesz, filesz);
            if (!(in->flags & LOAD_ELF_DO_LOAD))
                chunk->staging = get_elf_data_by_offset(in, offset, filesz);
            if (!chunk->destaddr)
                chunk->destaddr = vaddr;
            break;
        }
        case 2:
            ensure(type == PT_DYNAMIC);
            in->dynamic_off = offset;
            break;
        case 3:
            ensure(type == PT_NOTE);
            in->loader_comm_addr_unslid = vaddr;
        }
    }
    if (in->flags & LOAD_ELF_DO_LOAD) {
#if DUMMY
        for (size_t i = 0; i < LEI_NUM_CHUNKS; i++) {
            ensure(in->chunks[i].destaddr);
            in->chunks[i].staging = calloc(1, in->chunks[i].memsz);
        }
#else
        struct load_elf_info_chunk *text_chunk = &in->chunks[0];
        struct load_elf_info_chunk *data_chunk = &in->chunks[1];
        void *text_minus = tinyheap_alloc(g_shared_code_heap_tracking,
                                          text_chunk->memsz + text_chunk->misalignment,
                                          text_chunk->align);
        ensure(text_minus);
        void *text = (char *)text_minus + text_chunk->misalignment;
        text_chunk->destaddr = (uint32_t)text;
        text_chunk->staging = text;
        log("got text %p\n", text);

        void *data_staging
            = tinyheap_alloc(g_shared_read_heap_tracking, data_chunk->memsz, 4);
        ensure(data_staging);
        data_chunk->staging = data_staging;
        log("got ds %p\n", data_staging);

        data_chunk->destaddr
            = (((uint32_t)g_loader_shared->data_base + data_chunk->align - 1)
               & ~(data_chunk->align - 1))
            + data_chunk->misalignment;
        void *new_db
            = (void *)((data_chunk->destaddr + data_chunk->memsz + 0xfff) & ~0xfffu);
        priv_memcpy(&g_loader_shared->data_base, &new_db, sizeof(void *));
        add_shared_block(
            &g_loader_shared->copy_block_count, &g_loader_shared->copy_blocks,
            (struct loader_shared_block){(void *)data_chunk->destaddr,
                                         data_chunk->staging, 0, data_chunk->memsz});
#endif
    }
    for (size_t i = 0; i < LEI_NUM_CHUNKS; i++) {
        const struct load_elf_info_chunk *chunk = &in->chunks[i];
        if (LOAD_ELF_VERBOSITY >= 1)
            log("%s: %x..%x -> %x (slide=%#x) size=%x align=%x staging=%p\n",
                i ? "data" : "text", chunk->sourceaddr, chunk->sourceaddr + chunk->memsz,
                chunk->destaddr, chunk->destaddr - chunk->sourceaddr, chunk->memsz,
                chunk->align, chunk->staging);
    }
    if (in->flags & LOAD_ELF_DO_LOAD) {
        for (size_t i = 0; i < phnum; i++) {
            const Elf32_Phdr *ph = &phdrs[i];
            uint32_t vaddr = bswap(ph->p_vaddr), offset = bswap(ph->p_offset);
            uint32_t memsz = bswap(ph->p_memsz), filesz = bswap(ph->p_filesz);

            if (bswap(ph->p_type) == PT_LOAD) {
                volatile void *buf = get_staging_address(in, vaddr, memsz);
                void *tmp = MEMAllocFromDefaultHeap(filesz);
                ensure(tmp);
                const void *data = get_elf_data_by_offset(in, offset, filesz);
                memcpy(tmp, data, filesz);
                memcpy_code(buf, tmp, filesz, in);
                MEMFreeToDefaultHeap(tmp);
                bzero_code((char *)buf + filesz, memsz - filesz, in);
            }
        }
    }
}

static void
load_elf_dynamic(struct load_elf_info *in) {
    ensure(in->dynamic_off != -1);
    in->strtab_off = -1ul;
    in->symtab_off = -1ul;
    in->versym_off = -1ul;
    in->rela_off = -1ul;
    in->rela_size = 0;
    in->verneed_off = -1ul;
    in->verneed_count = 0;

    size_t cur_dynamic_off = in->dynamic_off;
    while (1) {
        const Elf32_Dyn *dynamic
            = (Elf32_Dyn *)get_elf_data_by_offset(in, cur_dynamic_off, sizeof(*dynamic));
        int32_t tag = bswap(dynamic->d_tag);
        uint32_t val = bswap(dynamic->d_un.d_val);
        switch (tag) {
        case DT_NULL:
            return;
        case DT_STRTAB:
            in->strtab_off = addr_to_off(in, val, 0);
            break;
        case DT_SYMTAB:
            in->symtab_off = addr_to_off(in, val, 0);
            break;
        case DT_RELA:
            in->rela_off = addr_to_off(in, val, 0);
            break;
        case DT_RELASZ:
            in->rela_size = val;
            break;
        case DT_RELAENT:
            ensure(val == 12);
            break;
        case DT_PLTGOT:
            in->plt_addr_unslid = val;
            break;
        case DT_VERSYM:
            in->versym_off = addr_to_off(in, val, 0);
            break;
        case DT_VERNEED:
            in->verneed_off = addr_to_off(in, val, 0);
            break;
        case DT_VERNEEDNUM:
            in->verneed_count = val;
            break;
        case DT_REL:
            ensure(false);
            break;
        }

        cur_dynamic_off += sizeof(Elf32_Dyn);
    }
}

static void
load_elf_verneed(struct load_elf_info *in) {
    if (in->versym_off == -1)
        return;
    ensure(in->strtab_off != -1);
    size_t strtab_maxsize;
    const char *strtab = (char *)get_max_elf_data_by_offset(in, in->strtab_off, &strtab_maxsize);
    ensure(in->versym_off != -1);
    size_t cur_verneed_off = in->verneed_off;
    ensure(cur_verneed_off != -1);
    for (size_t i = 0; i < in->verneed_count; i++) {
        const Elf32_Verneed *verneed
            = (Elf32_Verneed *)get_elf_data_by_offset(in, cur_verneed_off, sizeof(*verneed));
        ensure(bswap(verneed->vn_cnt) == 1);
        size_t cur_vernaux_off = cur_verneed_off + bswap(verneed->vn_aux);
        const Elf32_Vernaux *vernaux
            = (Elf32_Vernaux *)get_elf_data_by_offset(in, cur_vernaux_off, sizeof(*vernaux));
        size_t vna_other = bswap(vernaux->vna_other);
        ensure(vna_other < MAX_VNA_OTHER);
        const char *libname = get_dynstr(bswap(verneed->vn_file), strtab, strtab_maxsize);
#if !DUMMY
        if (LOAD_ELF_VERBOSITY >= 2)
            log("OSDynLoad_Acquire(%s)\n", libname);
        ensure(OS_SPECIFICS->p_OSDynLoad_Acquire(libname, &in->vna_other_to_lib[vna_other]) == 0);
#else
        in->vna_other_to_lib[vna_other] = (void *)libname;
#endif
        cur_verneed_off += bswap(verneed->vn_next);
    }
}

static void
make_plt_entry(struct load_elf_info *in, uint32_t out[2], uint32_t plt_slot_addr,
               uint32_t last_plt_slot_addr, uint32_t dst) {
    // really dumb
    uint32_t plt_addr = in->plt_addr_unslid;
    uint32_t plt_slot_idx = (plt_slot_addr - (plt_addr + 72)) / 8;
    uint32_t extra_word_addr = last_plt_slot_addr + 8 + 4 * plt_slot_idx;
    uint32_t extra_word_rel = extra_word_addr - plt_addr;
    ensure(extra_word_rel <= 0x7fff);
    out[0] = bswap(0x39600000u | extra_word_rel); // li r11, extra_word_rel
    out[1] = bswap(get_branch(plt_slot_addr + 4, plt_addr));
    // todo refactor
    void *extra_word_p = get_staging_address(in, extra_word_addr, 4);
    if (LOAD_ELF_VERBOSITY >= 2)
        log("plt_slot_addr=%x extra_word_addr=%p<-%x plt_addr=%x dst=%x out=%x %x\n",
            plt_slot_addr, extra_word_p, extra_word_addr, plt_addr, dst, out[0], out[1]);
    ensure(extra_word_p);
    dst = bswap(dst);
    memcpy_code(extra_word_p, &dst, sizeof(dst), in);
}

static void
add_plt_base_stub(struct load_elf_info *in) {
    uint32_t real_plt_addr = get_final_load_address(in, in->plt_addr_unslid);
    uint32_t real_plt_addr_lo = real_plt_addr & 0xffff;
    uint32_t real_plt_addr_ha
        = ((real_plt_addr >> 16) + !!(real_plt_addr_lo & 0x8000)) & 0xffff;
    uint32_t insns[] = {
        bswap(0x3d6b0000u | real_plt_addr_ha), // addis r11, r11, plt@ha
        bswap(0x816b0000u | real_plt_addr_lo), // lwz r11, plt@lo(r11)
        bswap(0x7d6903a6u), // mtctr r11
        bswap(0x4e800420u), // bctr
    };
    memcpy_code(get_staging_address(in, in->plt_addr_unslid, sizeof(insns)), insns,
                sizeof(insns), in);
}

static void
load_elf_rela(struct load_elf_info *in) {
    if (in->rela_off == -1)
        return;
    ensure(in->strtab_off != -1);
    ensure(in->symtab_off != -1);
    ensure(in->rela_off != -1);
    ensure(in->rela_size % sizeof(Elf32_Rela) == 0);
    size_t rela_count = in->rela_size / sizeof(Elf32_Rela);
    size_t strtab_maxsize;
    const char *strtab = (char *)get_max_elf_data_by_offset(in, in->strtab_off, &strtab_maxsize);
    size_t symtab_maxsize;
    Elf32_Sym *symtab = (Elf32_Sym *)get_max_elf_data_by_offset(in, in->symtab_off, &symtab_maxsize);
    size_t symtab_maxcount = symtab_maxsize / sizeof(*symtab);
    size_t versym_maxcount = 0;
    const Elf32_Rela *relas
        = (Elf32_Rela *)get_elf_data_by_offset(in, in->rela_off, sat_mul(sizeof(*relas), rela_count));
    const Elf32_Versym *versyms = nullptr;
    if (in->versym_off != -1) {
        size_t versym_maxsize;
        versyms = (Elf32_Versym *)get_max_elf_data_by_offset(in, in->versym_off, &versym_maxsize);
        versym_maxcount = versym_maxsize / sizeof(*versyms);
    }
    uint32_t max_jmp_slot_r_offset = 0;
    for (size_t i = 0; i < rela_count; i++) {
        const Elf32_Rela *rela = &relas[i];
        uint32_t r_offset = bswap(rela->r_offset);
        uint32_t r_info = bswap(rela->r_info);
        if (ELF32_R_TYPE(r_info) == R_PPC_JMP_SLOT)
            max_jmp_slot_r_offset = max(max_jmp_slot_r_offset, r_offset);
    }
    for (size_t i = 0; i < rela_count; i++) {
        const Elf32_Rela *rela = &relas[i];
        uint32_t r_offset = bswap(rela->r_offset);
        uint32_t r_info = bswap(rela->r_info);
        uint32_t r_addend = (uint32_t)bswap(rela->r_addend);
        uint32_t r_type = ELF32_R_TYPE(r_info);
        uint32_t r_sym = ELF32_R_SYM(r_info);
        uint32_t st_value;
        uint32_t magic_st_size = 0;
        bool have_magic_st_size = false;
        ensure(r_sym < symtab_maxcount);
        Elf32_Sym *sym = &symtab[r_sym];
        uint32_t st_info = bswap(sym->st_info);
        if (r_sym == 0 || bswap(sym->st_shndx) != STN_UNDEF) {
            if (!(in->flags & LOAD_ELF_DO_LOCAL_RELOCS))
                continue;
            uint32_t addr = r_addend;
            if (r_sym > 0 && ELF32_ST_TYPE(st_info) != STT_SECTION)
                addr += bswap(sym->st_value);

            st_value = get_final_load_address(in, addr);
        } else {
            if (!(in->flags & LOAD_ELF_DO_EXTERNAL_RELOCS))
                continue;
            uint32_t st_size = bswap(sym->st_size);
            if (!have_magic_st_size) {
                have_magic_st_size = true;
                magic_st_size = st_size ^ 0x80000000;
            }
            if (st_size != magic_st_size) {
                st_size = bswap(magic_st_size);
                const char *name = get_dynstr(bswap(sym->st_name), strtab, strtab_maxsize);
                ensure(r_sym < versym_maxcount);
                Elf32_Versym vna_other = bswap(versyms[r_sym]) & 0x7fff;
                ensure(vna_other < MAX_VNA_OTHER);
                void *lib = in->vna_other_to_lib[vna_other];
#if DUMMY
                printf("%s from %s\n", name, lib);
                ensure(lib != nullptr);
                st_value = 0x01deadea;
#else
                ensure(lib != nullptr);
                void *addr;
                int err = OS_SPECIFICS->p_OSDynLoad_FindExport(lib, true, name, &addr);
                if (err)
                    err = OS_SPECIFICS->p_OSDynLoad_FindExport(lib, false, name, &addr);
                if (err)
                    panic("failed to find export [%s]", name);
                st_value = (uint32_t)addr;
#endif
                sym->st_value = bswap(st_value);
            }
            st_value = bswap(sym->st_value) + r_addend;
        }
        union {
            uint16_t u16;
            uint32_t u32;
            uint32_t u32x2[2];
        } newval = {0};
        size_t size;
        switch (r_type) {
        case R_PPC_NONE:
            // ... what?
            continue;
        case R_PPC_RELATIVE:
        case R_PPC_ADDR32:
        case R_PPC_GLOB_DAT:
        case R_PPC_REL24:
            size = 4;
            break;
        case R_PPC_ADDR16_HA:
        case R_PPC_ADDR16_LO:
            size = 2;
            break;
        case R_PPC_JMP_SLOT:
            size = 8;
            break;
        default:
            panic("?r_type %d @ %x", r_type, r_offset);
        }
        uint32_t dstaddr = get_final_load_address(in, r_offset);
        volatile uint32_t *dst = (volatile uint32_t *)get_staging_address(in, r_offset, size);
        switch (r_type) {
        case R_PPC_RELATIVE:
        case R_PPC_ADDR32:
        case R_PPC_GLOB_DAT:
            newval.u32 = bswap(st_value);
            break;
        case R_PPC_ADDR16_HA:
            newval.u16 = bswap((uint16_t)((st_value >> 16) + ((st_value >> 15) & 1)));
            break;
        case R_PPC_ADDR16_LO:
            newval.u16 = bswap((uint16_t)(st_value & 0xffff));
            break;
        case R_PPC_REL24:
            newval.u32 = adjust_branch(*dst, dstaddr, st_value);
            break;
        case R_PPC_JMP_SLOT:
            make_plt_entry(in, newval.u32x2, r_offset, max_jmp_slot_r_offset, st_value);
            break;
        }
        if (LOAD_ELF_VERBOSITY >= 2)
            log("reloc: %p(0x%x) <- 0x%x,0x%x size=%u st_value=0x%x r_addend=0x%x\n", dst,
                dstaddr, newval.u32x2[0], newval.u32x2[1], (int)size, st_value, r_addend);
        memcpy_code(dst, &newval, size, in);
    }
    add_plt_base_stub(in);
}
#if 0
static void
load_hooks(struct load_elf_info *in) {
    log("hook_list=%p count=%d\n", in->hook_list, (int)in->hook_list_count);
    for (size_t i = 0; i < in->hook_list_count; i++) {
        const struct hook_info *hi = &in->hook_list[i];
    }
}
#endif

static void
load_elf(struct load_elf_info *in) {
    load_elf_header(in);
    load_elf_phdrs(in);
    load_elf_dynamic(in);
    load_elf_verneed(in);
    if (in->flags & (LOAD_ELF_DO_LOCAL_RELOCS | LOAD_ELF_DO_EXTERNAL_RELOCS))
        load_elf_rela(in);
}

#if !DUMMY
bool g_self_fixup_done = false;
static void
self_fixup(int more_flags) {
    struct load_elf_info in;
    xbzero(&in, sizeof(in));
    in.chunks[0].destaddr = (uint32_t)self_elf_start;
    in.chunks[0].in_buf = self_elf_start;
    in.chunks[0].filesz = -1u; // temp
    in.chunks[1].destaddr = (uint32_t)data_start;
    in.chunks[1].in_buf = data_start;
    in.chunks[1].filesz = -1u; // temp
    in.flags
        = LOAD_ELF_INPUT_USE_CHUNKWISE_IN_BUF | LOAD_ELF_DO_EXTERNAL_RELOCS | more_flags;
    load_elf(&in);
    g_self_fixup_done = true;
}
#endif

#if !DUMMY
extern "C" SDKCALL int main();
SDKCALL int
main() {
    // HBL should do this anyway, in a dumb way based on identifying the .bss *section*.
    // Redo it just in case someone tries to use this with a different loader
    extern char bss_start[], bss_end[];
    xbzero(bss_start, (size_t)(bss_end - bss_start));

    early_init();
    self_fixup(0);
    log_init(1234, LOG_ALL & ~LOG_USBDUCKS, nullptr);
    log("Hello!\n");
    //MEMAllocFromDefaultHeap(0xc800);
    if ((*(uint32_t *)FSOpenFile & 0xfc000000) == 0x48000000) {
        panic("Already hooked, idiot");
    }
    //install_exc_handler(OSSetExceptionCallbackEx);
    install_syscall((void *)my_syscall_impl);
    int test = my_syscall(MSM_TEST, 0, 0, 0); // test
    log("syscall OK test=>%x\n", test);

    //log("Hello!\n");
#if BONUS_FEATURES
    if (1) {
        if (!loud())
            return 0;
    }
#endif

    make_hbl_not_reload_me();
    patch_kernel_ibat0l_writes();
    enable_0100xxxx_kern_exec();
    // dump_loader_info();
    // return 0;
    dump_rpls();

    ensure(OSGetPFID() == 15); // so the hooks won't start in some other process
    // log("OSGetTitleID()=%llx\n", OSGetTitleID());
    ensure((OSGetTitleID() & ~0xfffull) == 0x000500101004A000);
    // need to make a copy in sane memory so OSEffectiveToPhysical works
    ensure(MEMAllocFromDefaultHeap);
    ensure(MEMFreeToDefaultHeap);

    struct load_elf_info in;
    xbzero(&in, sizeof(in));
    in.chunks[0].in_buf = self_elf_start;
    in.chunks[0].filesz = -1u; // temp
    in.chunks[1].in_buf = data_start;
    in.chunks[1].filesz = -1u; // temp
    in.flags = LOAD_ELF_INPUT_USE_CHUNKWISE_IN_BUF | LOAD_ELF_DO_LOAD | LOAD_ELF_USE_PRIV
        | LOAD_ELF_DO_LOCAL_RELOCS | LOAD_ELF_DO_EXTERNAL_RELOCS;
    load_elf(&in);

    ensure(in.loader_comm_addr_unslid);
    struct loader_comm *their_loader_comm = (struct loader_comm *)get_final_load_address(&in, in.loader_comm_addr_unslid);
    log("their_loader_comm = %p\n", their_loader_comm);
    ensure_eq(their_loader_comm->magic, LOADER_COMM_MAGIC);
    log("doing initial hook:\n");
    install_hooks(their_loader_comm->OSBlockThreadsOnExit_hook);
    log("all done\n");
    // log("main core: %d\n", OSIsMainCore());
    // log("hbm: %d\n", OSEnableHomeButtonMenu(1));
    uint64_t tids[] = {
        0x000500001018DB00,
        0x000500001018DC00,
        0x000500001018DD00,
    };
    bool launched = false;
    for (size_t i = 0; i < countof(tids); i++) {
        int ret = SYSCheckTitleExists(tids[i]);
        log("exists %llx: %d\n", tids[i], ret);
        if (ret) {
            ret = SYSLaunchTitle(tids[i]);
            log("launch %llx: %d\n", tids[i], ret);
            if (!ret) {
                launched = true;
                break;
            }
        }
    }
    if (!launched)
        log("SYSLaunchMenu: %d\n", SYSLaunchMenu());
    while (1) {
        struct OSMessage msg;
        if (OSReceiveMessage(OSGetSystemMessageQueue(), &msg, 1)) {
            log("got %x %x %x %x\n", msg.x[0], msg.x[1], msg.x[2], msg.x[3]);
            if (msg.x[1] == 0xFACEBACC) {
                log("releasing foreground\n");
                OSSavesDone_ReadyToRelease();
                ensure(OSRunThread(OSGetDefaultThread(0), (void *)OSReleaseForeground, 0, nullptr));
                ensure(OSRunThread(OSGetDefaultThread(2), (void *)OSReleaseForeground, 0, nullptr));
                OSReleaseForeground();
                log("OK???\n");
            } else if (msg.x[1] == 0xD1E0D1E0) {
                log("bye\n");
                exit();
            }
        } else {
            log("got nothing\n");
            OSYieldThread();
        }
    }
    // log("still no good...\n");
    /*
     */
    return 0;
}

template<> SDKCALL void
FUNC_HOOK_TY(OSSetExceptionCallback)::hook(int exc, bool (*SDKCALL cb)(OSContext *)) {
    if (in_right_process())
        return; // ignore game's exc callbacks
    return orig(exc, cb);
}
template<> SDKCALL void
FUNC_HOOK_TY(OSSetExceptionCallbackEx)::hook(int flag, int exc, bool (*SDKCALL cb)(OSContext *)) {
    if (in_right_process())
        return; // ignore game's exc callbacks
    return orig(flag, exc, cb);
}

static struct func_hook_info common_hooks_list[] = {
    FUNC_HOOK(OSSetExceptionCallbackEx),
    FUNC_HOOK(OSSetExceptionCallback),
    {0}
};


static struct atomic_u32 init_in_process_state;

static SDKCALL void
deinit_in_process(void) {
    uninstall_hooks(common_hooks_list);
}

static void
init_in_process() {
    install_exc_handler(OSSetExceptionCallbackEx);
    log_reset();
    enable_0100xxxx_kern_exec();
    install_syscall((void *)my_syscall_impl);

    // For the fourth(!) time, if you count patch-elf as first.
    // This fixes up references to non-system-shared (01xxxxxx) libraries.
    self_fixup(LOAD_ELF_USE_PRIV);

    struct usbducks *ud = nullptr;
#if ENABLE_USBDUCKS
    usbducks_init(&g_usbducks);
    usbducks_backend_lock(&g_usbducks);
    usbducks_start(&g_usbducks);
    usbducks_backend_unlock(&g_usbducks);
    ud = &g_usbducks;
#endif

    log_init(1235, LOG_ALL, ud);
    log("hello from mod (self_elf_start=%p)\n", self_elf_start);
    log_flush();

    dump_rpls();
    log("-->\n");

    install_hooks(common_hooks_list);
    static struct at_exit my_at_exit = {deinit_in_process};
    __ghs_at_exit(&my_at_exit);
#if ENABLE_GDBSTUB
    extern void startup_gdbstub(void);
    startup_gdbstub();
#endif
#if ENABLE_QMOD
    extern void startup_qmod(void);
    startup_qmod();
#endif
}

template<> SDKCALL void
FUNC_HOOK_TY(OSBlockThreadsOnExit)::hook(void) {
    orig();
    if (OSGetPFID() == 15 && MEMAllocFromDefaultHeap != nullptr) {
        while (load_acquire_atomic_u32(&init_in_process_state) != 2) {
            if (!OSCompareAndSwapAtomic(&init_in_process_state, 0, 1))
                continue;
            init_in_process();
            uninstall_hooks(OSBlockThreadsOnExit_hook);
            OSMemoryBarrier();
            store_release_atomic_u32(&init_in_process_state, 2);
        }
    }
}

bool
in_right_process() {
    if (OSGetPFID() != 15) {
        // We are in a non-game process, and the global variables are probably
        // garbage (because the heap wasn't bumped up :v).
        return false;
    }
    return load_acquire_atomic_u32(&init_in_process_state) == 2;
}

__attribute__((section(".loader_comm"), used))
const struct loader_comm loader_comm = {
    LOADER_COMM_MAGIC,
    OSBlockThreadsOnExit_hook
};

const struct func_hook_info OSBlockThreadsOnExit_hook[] = {
    FUNC_HOOK(OSBlockThreadsOnExit),
    {0}
};

#endif // !DUMMY

END_LOCAL_DECLS
