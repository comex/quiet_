#include "decls.h"
#include "logging.h"

#if IDENTIFYTEST
#include <stdio.h>
static uint32_t text_segment_addr, text_segment_size;
static void *text_segment_data;
#endif

static inline uint32_t
opc_stw(uint32_t rs, uint32_t d, uint32_t ra) {
    return (36u << 26) | (rs << 21) | (ra << 16) | d;
}
static inline uint32_t
opc_stb(uint32_t rs, uint32_t d, uint32_t ra) {
    return (38u << 26) | (rs << 21) | (ra << 16) | d;
}
static inline uint32_t
opc_addi(uint32_t rd, uint32_t ra, uint32_t simm) {
    return (14u << 26) | (rd << 21) | (ra << 16) | simm;
}
static const uint32_t opcode_mask = 31u << 26;

static inline uint32_t
read32(uint32_t addr) {
#if IDENTIFYTEST
    uint32_t off = addr - text_segment_addr;
    ensure(off < text_segment_size && (text_segment_size - off) >= 4);
    ensure(!(addr & 3));
    return bswap(*(uint32_t *)(text_segment_data + off));
#else
    return *(uint32_t *)addr;
#endif
}

struct candidate_list {
    uint32_t count;
    uint32_t addrs[1];
};

struct candidate_search {
    struct candidate_list pat1_list;
};

static void
add_candidate(struct candidate_list *cl, uint32_t addr) {
    size_t i = cl->count++;
    if (i < countof(cl->addrs))
        cl->addrs[i] = addr;
}

__attribute__((cold)) static void
check_pat1(struct candidate_search *cs, uint32_t addr) {
    if ((read32(addr - 4) & (opcode_mask | opc_stw(0, 0xffff, 0))) == opc_stw(0, 0x14, 0)
        && (read32(addr + 4) & (opcode_mask | opc_stb(0, 0xffff, 0)))
            == opc_stb(0, 0xc, 0)) {
        add_candidate(&cs->pat1_list, addr);
    }
}

__attribute__((always_inline)) static inline void
check_addr(struct candidate_search *cs, uint32_t addr, uint32_t val) {
    if ((val & (opcode_mask | opc_addi(0, 31, 0xffff))) == opc_addi(0, 0, 0x79)) {
        check_pat1(cs, addr);
    }
}

static void
check_addrs(struct candidate_search *cs, uint32_t addr, uint32_t len) {
    ensure((addr & 3) == 0);
    ensure((len & 3) == 0);
    while ((addr & 0xc) && len) {
        check_addr(cs, addr, read32(addr));
        addr += 4;
        len -= 4;
    }
    while (len >= 0x10) {
        uint32_t val0 = read32(addr);
        uint32_t val4 = read32(addr + 4);
        uint32_t val8 = read32(addr + 8);
        uint32_t valc = read32(addr + 0xc);
        check_addr(cs, addr + 0, val0);
        check_addr(cs, addr + 4, val4);
        check_addr(cs, addr + 8, val8);
        check_addr(cs, addr + 0xc, valc);
        addr += 0x10;
        len -= 0x10;
    }
    while (len) {
        check_addr(cs, addr, read32(addr));
        addr += 4;
        len -= 4;
    }
}

static bool
find_bl(uint32_t addr, uint32_t end, uint32_t *target_p) {
    for (; addr < end; addr += 4) {
        uint32_t val = read32(addr);
        if ((val & (opcode_mask | 0b11)) == (18u << 26 | 0b01)) {
            uint32_t masked = val & 0x03fffffcu;
            uint32_t extended = masked | ((masked & 0x02000000) ? 0xfc000000 : 0);
            *target_p = addr + extended;
            return true;
        }
    }
    return false;
}

static inline const char *
do_search(uint32_t addr, size_t len) {
    struct candidate_search cs = {0};
    check_addrs(&cs, addr, len);
    if (cs.pat1_list.count > 1)
        return "failed to find pat1 (multiple candidates)";
    else if (cs.pat1_list.count == 0)
        return "failed to find pat1 (no candidates)";

    uint32_t pat1_addr = cs.pat1_list.addrs[0];
    uint32_t get_default_version;
    if (!find_bl(pat1_addr, addr + len, &get_default_version))
        return "failed to find BL after pat1";
    printf("%x\n", get_default_version);

    return NULL; // no error
}

#if IDENTIFYTEST
int
main(int argc, char **argv) {
    const char *filename = argv[1];
    ensure(filename);
    FILE *fp = fopen(filename, "rb");
    ensure(fp);
    ensure(!fseek(fp, 0, SEEK_END));
    off_t size = ftello(fp);
    ensure(!fseek(fp, 0, SEEK_SET));
    text_segment_addr = 0x02000020;
    text_segment_size = (uint32_t)size;
    text_segment_data = malloc(size);
    ensure(fread(text_segment_data, 1, text_segment_size, fp) == text_segment_size);
    const char *err = do_search(text_segment_addr, text_segment_size);
    if (err) {
        printf("err: %s\n", err);
    } else {
        printf("OK\n");
    }
}
#endif
