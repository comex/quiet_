#pragma once
#if !DUMMY

#include "decls.h"
#include "types.h"

// this is really dumb
struct hbl_OsSpecifics {
    typeof(OSDynLoad_Acquire) *p_OSDynLoad_Acquire;
    typeof(OSDynLoad_FindExport) *p_OSDynLoad_FindExport;
    void *OSTitle_main_entry;
    void *kern_syscall_tbl[5];

    void *LiWaitIopComplete;
    void *LiWaitIopCompleteWithInterrupts;
    void *LiWaitOneChunk;
    void *PrepareTitle_hook;
    void *sgIsLoadingBuffer;
    void *gDynloadInitialized;
    uint32_t orig_LiWaitOneChunkInstr;
};

#define OS_SPECIFICS ((struct hbl_OsSpecifics *)0x00801500)
#define ELF_DATA_ADDR (*(uint32_t *)0x00801300)
#define MEM_AREA_TABLE ((void *)0x00801600)
#define MAIN_ENTRY_ADDR (*(uint32_t *)0x00801400)

enum my_syscall_mode {
    MSM_INVAL_IC,
    MSM_MEMCPY,
    MSM_TEST,
    MSM_ENABLE_IBAT0_KERN_EXEC,
};

int my_syscall_impl(uint32_t mode, uint32_t dst, uint32_t src, uint32_t len);

int my_syscall(enum my_syscall_mode mode, uint32_t dst, uint32_t src, uint32_t len);

void install_syscall(void *the_syscall_impl);

size_t priv_try_memcpy(volatile void *dst, const volatile void *src, size_t len);
void priv_memcpy(volatile void *dst, const volatile void *src, size_t len);

void patch_security_level(void);

void patch_kernel_devmode(void);
void patch_kernel_ibat0l_writes(void);
void patch_tracestub(void);
void dump_rpls(void);

bool find_rpl_info(struct rpl_info *out, const char *name_suffix);

void enable_0100xxxx_kern_exec(void);

#endif // DUMMY
