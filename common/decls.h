#pragma once
#include "types.h"

#if DUMMY
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <atomic>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>
#include <thread>
#include <mutex>
#include <future>
#include <fcntl.h>
#endif

typedef struct {
    uint32_t held;
    uint32_t trigger;
    uint32_t release;
    char x[0xac - 3 * 4];
} VPADStatus;

struct voice;
struct ve {
    uint16_t cur;
    int16_t delta;
};

struct OSThread;
typedef void (*thread_deallocator_cb)(OSThread *, void *);

#if !DUMMY
typedef uint32_t fd_set;
#endif
struct ioctl_select_params {
    int nfds;
    fd_set readfds;
    fd_set writefds;
    fd_set errfds;
    uint64_t timeout;
    int has_timeout;
} __attribute__((packed));
#if !DUMMY
CHECK_SIZE(struct ioctl_select_params, 0x1c);
#endif

struct rpl_info {
    const char *name;
    uint32_t text_addr, text_slide, text_size;
    uint32_t data_addr, data_slide, data_size;
    uint32_t rodata_addr, rodata_slide, rodata_size;
};

#if !DUMMY
#define memory_order_acquire __ATOMIC_ACQUIRE
#define memory_order_release __ATOMIC_RELEASE
#define memory_order_relaxed __ATOMIC_RELAXED
#define memory_order_seq_cst __ATOMIC_SEQ_CST

template <typename T>
struct atomic {
    constexpr atomic() {}
    explicit constexpr atomic(T val) : val(val) {}
    T val;
};

template <typename T, typename U>
static inline void atomic_store_explicit(atomic<T> *obj, U desr, int order) {
    T desr_real = desr;
#ifdef __clang__
    __c11_atomic_store((_Atomic(T) *)&obj->val, desr_real, order);
#else
    __atomic_store(&obj->val, &desr_real, order);
#endif
}

template <typename T>
static inline T atomic_load_explicit(atomic<T> *obj, int order) {
#ifdef __clang__
    return __c11_atomic_load((_Atomic(T) *)&obj->val, order);
#else
    T val;
    __atomic_load(&obj->val, &val, order);
    return val;
#endif
}

extern "C" {

#define offsetof __builtin_offsetof

struct MEMAllocator {
    uint32_t x[4];
};

extern void *(*MEMAllocFromDefaultHeap)(size_t size);
extern void *(*MEMAllocFromDefaultHeapEx)(size_t size, int align);
extern void (*MEMFreeToDefaultHeap)(void *ptr);
extern void MEMInitAllocatorForDefaultHeap(struct MEMAllocator *allocator);
extern void *MEMAllocFromExpHeapEx(void *exp_heap, size_t size, int align);

extern void DCInvalidateRange(void *ptr, size_t len);
extern void DCFlushRange(const void *ptr, size_t len);
extern void ICInvalidateRange(const void *ptr, size_t len);

extern struct voice *AXAcquireVoice(int p, void (*yoink_cb)(void *), void *yoink_ctx);
extern struct voice *AXAcquireVoiceEx(int p, void (*yoink_cb)(void *, uint32_t, uint32_t),
                                      void *yoink_ctx);
extern void AXSetVoiceType(struct voice *voice, int type);
extern void AXFreeVoice(struct voice *voice);
extern void AXSetVoiceVe(struct voice *voice, struct ve *ve);
extern void AXSetVoiceVeDelta(struct voice *voice, int16_t delta);
extern int AXUserBegin(void);
extern int AXUserEnd(void);
extern int AXUserIsProtected(void);
extern int AXRegisterAppFrameCallback(void (*callback)(void));
extern int AXDeregisterAppFrameCallback(void (*callback)(void));

extern int VPADRead(int controller, VPADStatus *buf, uint32_t length, int *err);

extern int OSIsMainCore(void);

extern int OSGetMemBound(int type, void **addr, size_t *size);
extern int OSGetForegroundBucket(void **addr, size_t *size);
extern uint32_t OSEffectiveToPhysical(const volatile void *addr);
extern void OSSleepTicks(uint64_t ticks);

typedef struct {
    uint32_t x[9];
} OSEvent;
extern void OSInitEvent(OSEvent *event, bool initial, bool autoreset);
extern void OSSignalEvent(OSEvent *event);
extern void OSWaitEvent(OSEvent *event);
extern bool OSWaitEventWithTimeout(OSEvent *event, uint64_t timeout);
extern void OSResetEvent(OSEvent *event);

struct OSThread;
typedef struct {
    uint32_t x[7];
    struct OSThread *owner;
    uint32_t y[3];
} OSMutex;
extern void OSInitMutex(OSMutex *mutex);
extern void OSLockMutex(OSMutex *mutex);
extern void OSUnlockMutex(OSMutex *mutex);

typedef struct {
    uint32_t x[7];
} OSCond;
extern void OSInitCond(OSCond *cond);
extern void OSWaitCond(OSCond *cond, OSMutex *mutex);
extern void OSSignalCond(OSCond *cond);

typedef struct OSContext {
    char tag[8];
    uint32_t gpr[32];
    uint32_t cr;
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t ex0;
    uint32_t ex1;
    uint32_t exception_type;
    uint32_t x;
    uint32_t x2;
    uint32_t fpscr;
    double fpr[32];
    uint16_t y;
    uint16_t flags;
    char pad[0x1dc - 0x1bc];
    uint32_t core;
    char pad2[0x304 - 0x1e0];
    uint32_t attr;
} __attribute__((aligned(8))) OSContext;
CHECK_SIZE(OSContext, 0x308);

typedef struct OSThread OSThread;

struct thread_link {
    OSThread *prev, *next;
};

struct OSThread {
    struct OSContext context;
    char pad[0x324 - sizeof(struct OSContext)];
    uint8_t state;
    uint8_t attr; // another attr field
    uint16_t tid;
    uint32_t suspend_count;
    uint32_t priority;
    char pad3[0x338 - 0x330];
    struct thread_link *run_queue[3];
    struct thread_link run_link[3];
    char pad4[0x38c - 0x35c];
    struct thread_link active_link;
    void *stack_start, *stack_end;
    char pad5[0x57c - 0x39c];
    void *thread_specific[16];
    char pad6[0x5d8 - 0x5bc];
    uint32_t pending_stop_reason;
    uint32_t pending_suspend_count;
    char pad7[0x6a0 - 0x5e0];
};
CHECK_SIZE(OSThread, 0x6a0);

extern void OSBlockThreadsOnExit(void);
extern void OSYieldThread(void);
extern int OSCreateThread(OSThread *thread, void *entry, int arg, void *arg2, void *stack,
                          int stack_size, int prio, int attr);
extern int OSCreateThreadType(OSThread *thread, void *entry, int arg, void *arg2,
                              void *stack, int stack_size, int prio, int attr, int type);
extern int __OSCreateThreadType(OSThread *thread, void *entry, int arg, void *arg2,
                                void *stack, int stack_size, int prio, int attr,
                                int type);
extern int OSRunThread(void *thread, void *entry, int arg, void *arg2);
extern OSThread *OSGetDefaultThread(int core);
extern OSThread *OSGetCurrentThread(void);
extern int OSGetCoreId();
extern void OSExitThread(int ret);
extern bool OSJoinThread(OSThread *thread, int *retp);
extern void OSSetThreadSpecific(int i, void *val);
extern int OSSuspendThread(OSThread *thread);
extern void __OSSuspendThreadNolock(OSThread *thread);
extern int OSResumeThread(OSThread *thread);
extern const char *OSGetThreadName(const OSThread *thread);
extern void OSSetThreadName(OSThread *thread, const char *name);
extern thread_deallocator_cb OSSetThreadDeallocator(OSThread *thread,
                                                    thread_deallocator_cb cb);
extern bool OSSetThreadAffinity(OSThread *thread, int affinity);

typedef struct OSThreadQueue {
    uint32_t x[4];
} OSThreadQueue;
extern void OSInitThreadQueue(OSThreadQueue *queue);
extern void OSWakeupThread(OSThreadQueue *queue);

extern void OSRequestFastExit(void);

struct OSMessage {
    uint32_t x[4];
};
extern int OSReceiveMessage(void *mq, struct OSMessage *msg, int flag);
extern void *OSGetSystemMessageQueue(void);
extern void OSSavesDone_ReadyToRelease(void);
extern void OSReleaseForeground(void);
extern void exit(void);

struct fs_client {
    char buf[0x1700];
} __attribute__((aligned(4)));
struct fs_cmd {
    char buf[0xa80];
} __attribute__((aligned(4)));
struct async_callback {
    void (*callback)(struct fs_client *, struct fs_cmd *, int, void *);
    void *ctx;
    void *mq;
};

extern void FSInit(void);
extern void FSInitCmdBlock(struct fs_cmd *cmd);
extern int FSOpenFile(struct fs_client *client, struct fs_cmd *cmd, const char *path,
                      const char *mode, int *handle, uint32_t flags);
extern int FSOpenFileAsync(struct fs_client *client, struct fs_cmd *cmd, const char *path,
                           const char *mode, int *handle, uint32_t flags,
                           const struct async_callback *callback);
extern int FSCloseFile(struct fs_client *client, struct fs_cmd *cmd, int handle,
                       uint32_t flags);
extern int FSReadFile(struct fs_client *client, struct fs_cmd *cmd, void *ptr,
                           size_t size, size_t nitems, int handle, uint32_t flags1,
                           uint32_t flags2);
extern int FSReadFileWithPos(struct fs_client *client, struct fs_cmd *cmd, void *ptr,
                           size_t size, size_t nitems, uint32_t pos, int handle, uint32_t flags1,
                           uint32_t flags2);
extern int FSReadFileAsync(struct fs_client *client, struct fs_cmd *cmd, void *ptr,
                           size_t size, size_t nitems, int handle, uint32_t flags1,
                           uint32_t flags2, const struct async_callback *callback);
extern int FSWriteFile(struct fs_client *client, struct fs_cmd *cmd, const void *ptr,
                       size_t size, size_t nitems, int handle, uint32_t flags1,
                       uint32_t flags2);
extern int FSWriteFileAsync(struct fs_client *client, struct fs_cmd *cmd, const void *ptr,
                            size_t size, size_t nitems, int handle, uint32_t flags1,
                            uint32_t flags2, const struct async_callback *callback);
extern int FSAWriteFile(int fsah, const void *ptr, size_t size, size_t nitems, int handle,
                        uint32_t flags);
extern int FSCloseFileAsync(struct fs_client *client, struct fs_cmd *cmd, int handle,
                            uint32_t flags, const struct async_callback *callback);
extern int FSAddClient(struct fs_client *client, uint32_t flags);

extern int SAVEOpenFileAsync(struct fs_client *client, struct fs_cmd *cmd, int slot,
                             const char *path, const char *mode, int *handle, int flags,
                             const struct async_callback *callback);
extern int SAVEInitSaveDir(int slot);
extern int SAVEFlushQuotaAsync(struct fs_client *client, struct fs_cmd *cmd, int slot,
                               int flags, const struct async_callback *callback);

__attribute__((format(printf, 3, 4))) extern void OSPanic(const char *file, int line,
                                                          const char *fmt, ...);

struct atomic_u64 {
    atomic<uint64_t> val;
};
extern uint64_t OSAddAtomic64(struct atomic_u64 *v, uint64_t add);
extern uint64_t OSSwapAtomic64(struct atomic_u64 *v, uint64_t value);

extern bool OSCompareAndSwapAtomic(atomic<uint32_t> *v, uint32_t old, uint32_t nu);
extern uint32_t OSAddAtomic(atomic<uint32_t> *v, uint32_t add);

extern void OSMemoryBarrier(void);

int64_t OSGetTime(void);

struct system_info {
    uint32_t clock;
};

struct system_info *OSGetSystemInfo(void);

extern int OSGetSecurityLevel(void);
extern int OSDynLoad_Acquire(const char *name, void **handle);
extern int OSDynLoad_FindExport(void *handle, bool data, const char *symbol,
                                void **address);
extern void OSDynLoad_Release(void *handle);
extern int OSDynLoad_GetNumberOfRPLs(void);
extern int OSDynLoad_GetRPLInfo(int first, int count, struct rpl_info *infos);

extern void OSLogRetrieve(int flag, void *buffer, size_t size);

extern void OSFatal(const char *msg);

__attribute__((format(printf, 3, 4))) extern int __os_snprintf(char *buf, size_t len,
                                                               const char *fmt, ...);

struct in_addr {
    uint32_t s_addr;
};
struct sockaddr;
struct sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;
    struct in_addr sin_addr;
    char sin_zero[8];
};
typedef int socklen_t;
extern int inet_aton(const char *cp, struct in_addr *pin);
extern int socket_lib_init(void);
extern int socket(int family, int type, int proto);
extern int listen(int socket, int backlog);
extern int bind(int socket, const struct sockaddr *address, socklen_t address_len);
extern int accept(int socket, struct sockaddr *address, socklen_t *address_len);
extern int connect(int socket, const void *address, int address_len);
extern ssize_t send(int socket, const void *buffer, size_t length, int flags);
extern ssize_t recv(int socket, void *buffer, size_t length, int flags);
#define SOL_SOCKET -1
#define SO_NBIO 0x1014
extern int setsockopt(int socket, int level, int option_name, void *option_value,
                      socklen_t option_len);
extern int socketclose(int socket);
extern int socketlasterr(void);
extern char *inet_ntoa(struct in_addr in);
struct timeval;
extern int select(int nfds, fd_set *readfds, fd_set *writefds, fd_set *errorfds,
                  struct timeval *timeout);

enum { FD_SETSIZE = 32 };
static inline void
FD_ZERO(fd_set *set) {
    *set = 0;
}
static inline void
FD_SET(int fd, fd_set *set) {
    *set |= 1u << fd;
}
static inline bool
FD_ISSET(int fd, fd_set *set) {
    return *set & 1u << fd;
}

static inline uint16_t
htons(uint16_t x) {
    return x;
}
static inline uint32_t
htonl(uint32_t x) {
    return x;
}

#define INADDR_ANY 0
#define AF_INET 2
#define SOCK_STREAM 1
#define IPPROTO_TCP 6

extern int netconf_init(void);
extern int netconf_get_assigned_address(int interface, struct in_addr *addr);

struct ioctlv {
    void *addr;
    size_t len;
    void *aux;
};

extern int get_socket_rm_fd(void);
extern int IOS_IoctlAsync(int handle, uint32_t req, const void *in, size_t in_len,
                          void *out, size_t out_len, void (*callback)(int ret, void *ctx),
                          void *ctx);

struct uhs_client {
    int state;
    int x[0x20 / 4];
};

struct uhs_client_opts {
    int which;
    void *buf;
    size_t buf_size;
};

// based on https://github.com/wiiudev/libwiiu/blob/master/libwiiu/src/uhs.h

/* Determines which interface parameters to check */
#define MATCH_DEV_VID 0x001
#define MATCH_DEV_PID 0x002
#define MATCH_DEV_CLASS 0x010
#define MATCH_DEV_SUBCLASS 0x020
#define MATCH_DEV_PROTOCOL 0x040
#define MATCH_IF_CLASS 0x080
#define MATCH_IF_SUBCLASS 0x100
#define MATCH_IF_PROTOCOL 0x200

/* Endpoint transfer directions */
#define ENDPOINT_TRANSFER_OUT 1
#define ENDPOINT_TRANSFER_IN 2

struct UhsInterfaceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bInterfaceNumber;
    uint8_t bAlternateSetting;
    uint8_t bNumEndpoints;
    uint8_t bInterfaceClass;
    uint8_t bInterfaceSubClass;
    uint8_t bInterfaceProtocol;
    uint8_t iInterface;
} __attribute__((packed));

struct UhsEndpointDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint8_t bEndpointAddress;
    uint8_t bmAttributes;
    uint16_t wMaxPacketSize;
    uint8_t bInterval;
    char extra[2];
} __attribute__((packed));

struct UhsDeviceDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t bcdUSB;
    uint8_t bDeviceClass;
    uint8_t bDeviceSubClass;
    uint8_t bDeviceProtocol;
    uint8_t bMaxPacketSize0;
    uint16_t idVendor;
    uint16_t idProduct;
    uint16_t bcdDevice;
    uint8_t iManufacturer;
    uint8_t iProduct;
    uint8_t iSerialNumber;
    uint8_t bNumConfigurations;
} __attribute__((packed));

struct UhsConfigDescriptor {
    uint8_t bLength;
    uint8_t bDescriptorType;
    uint16_t wTotalLength;
    uint8_t bNumInterfaces;
    uint8_t bConfigurationValue;
    uint8_t iConfiguration;
    uint8_t bmAttributes;
    uint8_t bMaxPower;
} __attribute__((packed));

struct UhsInterfaceFilter {
    uint16_t match_params; /* Bitmask of above flags */
    uint16_t vid, pid; /* Vendor ID and product ID */
    char unknown6[0xa - 0x6];
    uint8_t dev_class; /* Device class */
    uint8_t dev_subclass; /* Device subclass */
    uint8_t dev_protocol; /* Device protocol */
    uint8_t if_class; /* Interface class */
    uint8_t if_subclass; /* Interface subclass */
    uint8_t if_protocol; /* Interface protocol */
} __attribute__((packed));
CHECK_SIZE(struct UhsInterfaceFilter, 0x10);

struct UhsInterfaceProfile {
    uint32_t if_handle;
    char unknown4[0x14 - 0x4];
    uint16_t ports[8];
    uint32_t num_alt_settings;
    struct UhsDeviceDescriptor dev_desc;
    struct UhsConfigDescriptor cfg_desc;
    struct UhsInterfaceDescriptor intf_desc;
    struct UhsEndpointDescriptor endpoints[32];
} __attribute__((packed));
CHECK_SIZE(struct UhsInterfaceProfile, 0x16c);

extern int UhsClientOpen(struct uhs_client *c, struct uhs_client_opts *o);
extern int UhsQueryInterfaces(struct uhs_client *c, struct UhsInterfaceFilter *filter,
                              struct UhsInterfaceProfile *profiles, size_t num_profiles);
extern int UhsAcquireInterface(struct uhs_client *c, uint32_t if_handle, void *user,
                               void (*callback)(void *user, uint32_t if_handle,
                                                uint32_t event));
extern int UhsAdministerEndpoint(struct uhs_client *c, uint32_t if_handle, int mode,
                                 uint32_t ep_mask, uint32_t count, uint32_t size);
extern int UhsSubmitBulkRequestAsync(
    struct uhs_client *c, uint32_t if_handle, uint8_t endpoint, int direction,
    const void *buffer, size_t length, void *user,
    void (*callback)(void *user, int ret, uint32_t if_handle, uint8_t endpoint,
                     int direction, const void *buffer, size_t size));
extern int UhsSubmitControlRequest(struct uhs_client *c, uint32_t if_handle, void *data,
                                   uint8_t bRequest, uint8_t bRequestType,
                                   uint16_t wValue, uint16_t wIndex, uint16_t wLength,
                                   int timeout);

enum {
    DSI = 2,
    ISI = 3,
    PROGRAM = 6,
};
#define ALL_CORES 4

struct platform_info {
    uint32_t unk_0;
    uint32_t unk_4;
    uint32_t flags;
    uint32_t console_type;
    uint32_t unk_10[0xff];
};
CHECK_SIZE(struct platform_info, 0x40c);
extern struct platform_info __OSPlatformInfo;

extern int OSSetDABR(bool all_cores, uint32_t addr, bool dr, bool dw);
extern int OSSetIABR(bool all_cores, uint32_t addr);
extern void OSSetExceptionCallback(int exc, bool (*cb)(OSContext *));
extern void OSSetExceptionCallbackEx(int flag, int exc, bool (*cb)(OSContext *));

extern int OSGetPFID(void);
extern uint64_t OSGetTitleID(void);
extern int __OSGetTitleVersion();

extern void OSScreenInit(void);
extern size_t OSScreenGetBufferSizeEx(int screen);
extern void OSScreenSetBufferEx(int screen, void *buf);
extern void OSScreenEnableEx(int screen, bool enabled);
extern void OSScreenClearBufferEx(int screen, uint32_t color);
extern void OSScreenPutFontEx(int screen, int x, int y, const char *str);
extern void OSScreenFlipBuffersEx(int screen);

extern int OSEnableHomeButtonMenu(int enabled);

extern void OSConsoleWrite(const char *s, size_t len);
extern void __OSConsoleWrite(const char *s, size_t len);
__attribute__((format(printf, 2, 3)))
extern void COSError(int x, const char *fmt, ...);

extern void __OSLockScheduler(OSThread *thread);
extern void __OSUnlockScheduler(OSThread *thread);
extern uint32_t __OSSchedulerLock;
extern int OSDisableInterrupts(void);
extern void OSRestoreInterrupts(int old);
//extern int OSIsInterruptEnabled(void);
static inline int OSIsInterruptEnabled(void) {
    return *(int *)0xFFFFFFE4;
}

extern struct OSContext *OSGetCurrentContext(void);
extern void OSInitContext(struct OSContext *ctx);
extern void OSLoadContext(struct OSContext *ctx);
extern void OSSaveContext(struct OSContext *ctx);

extern int OSLaunchTitlev(uint64_t tid, int argc, const char **argv);
extern int OSSendAppSwitchRequest(int pfid, const char *arg, size_t len);
extern int SYSLaunchTitle(uint64_t tid);
extern int SYSLaunchMenu(void);
extern int SYSCheckTitleExists(uint64_t tid);

struct at_exit {
    void (*f)(void);
    struct at_exit *next;
};
extern void __ghs_at_exit(struct at_exit *at_exit);

#define GX2EndianSwapMode_Default 3
#define GX2AttribFormat_FLOAT_32_32 0x80d
#define GX2AttribFormat_UNORM_8_8_8_8 0xa
#define GX2SurfaceFormat_UNORM_R8 1
#define GX2SurfaceFormat_UNORM_R8_G8_B8_A8 0x1a
#define GX2FetchShaderType_NoTessellation 0
#define GX2RResourceFlags_BindTexture 1
#define GX2RResourceFlags_BindVertexBuffer 0x10
#define GX2RResourceFlags_UsageCpuReadWrite 0x1800
#define GX2RResourceFlags_UsageGpuRead 0x2000
#define GX2RResourceFlags_UsageGpuWrite 0x4000
#define GX2RResourceFlags_UsageGpuReadWrite 0x6000
#define GX2TexClampMode_ClampBorder 6
#define GX2TexXYFilterMode_Point 0
#define GX2TexBorderType_Black 1
#define GX2TexBorderType_White 2
#define GX2BlendMode_Zero 0
#define GX2BlendMode_One 1
#define GX2BlendMode_SrcAlpha 4
#define GX2BlendMode_InvSrcAlpha 5
#define GX2BlendMode_BlendFactor 13
#define GX2BlendCombineMode_Add 0
#define GX2LogicOp_Copy 0xcc
#define GX2PrimitiveMode_Rects 0x11
#define GX2InvalidateMode_CPU 0x40
#define GX2InvalidateMode_Shader 0x8
#define GX2QueryType_OcclusionQuery 0
#define GX2TileMode_LinearAligned 1
#define GX2TileMode_LinearSpecial 16

struct GX2QueryData {
    uint32_t a[0x10];
};
CHECK_SIZE(struct GX2QueryData, 0x40);

struct GX2AttribStream {
    uint32_t location;
    uint32_t buffer;
    uint32_t offset;
    uint32_t format;
    uint32_t type;
    uint32_t aluDivisor;
    uint32_t mask;
    uint32_t endianSwap;
};
CHECK_SIZE(struct GX2AttribStream, 0x20);

struct GX2Surface {
    uint32_t dim;
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mipLevels;
    uint32_t format;
    uint32_t aa;
    uint32_t resourceFlags;
    uint32_t imageSize;
    void *image;
    uint32_t mipmapSize;
    void *mipmaps;
    uint32_t tileMode;
    uint32_t swizzle;
    uint32_t alignment;
    uint32_t pitch;
    uint32_t mipLevelOffset[13];
};
CHECK_SIZE(struct GX2Surface, 0x74);

struct GX2Texture {
    struct GX2Surface surface;
    uint32_t viewFirstMip;
    uint32_t viewNumMips;
    uint32_t viewFirstSlice;
    uint32_t viewNumSlices;
    uint32_t compMap;
    struct {
        uint32_t word0;
        uint32_t word1;
        uint32_t word4;
        uint32_t word5;
        uint32_t word6;
    } regs;
};
CHECK_SIZE(struct GX2Texture, 0x9c);

struct GX2DepthBuffer {
    struct GX2Surface surface;
    uint32_t viewMip;
    uint32_t viewFirstSlice;
    uint32_t viewNumSlices;
    void *hiZPtr;
    uint32_t hiZSize;
    float depthClear;
    uint32_t stencilClear;
    struct {
        uint32_t db_depth_size;
        uint32_t db_depth_view;
        uint32_t db_depth_info;
        uint32_t db_htile_surface;
        uint32_t db_prefetch_limit;
        uint32_t db_preload_control;
        uint32_t pa_poly_offset_cntl;
    } regs;
};
CHECK_SIZE(struct GX2DepthBuffer, 0xac);

struct GX2ColorBuffer {
    struct GX2Surface surface;
    uint32_t viewMip;
    uint32_t viewFirstSlice;
    uint32_t viewNumSlices;
    void *aaBuffer;
    uint32_t aaSize;

    struct {
        uint32_t cb_color_size;
        uint32_t cb_color_info;
        uint32_t cb_color_view;
        uint32_t cb_color_mask;
        uint32_t cmask_offset;
    } regs;
};
CHECK_SIZE(struct GX2ColorBuffer, 0x9c);

struct GX2FetchShader {
    uint32_t type;

    uint32_t sq_pgm_resources_fs;

    uint32_t size;
    void *data;
    uint32_t attribCount;
    uint32_t numDivisors;
    uint32_t divisors[2];
};
CHECK_SIZE(struct GX2FetchShader, 0x20);

struct GX2UniformVar {
    const char *name;
    uint32_t type;
    uint32_t count;
    uint32_t offset;
    int32_t block;
};
CHECK_SIZE(struct GX2UniformVar, 0x14);

struct GX2UniformInitialValue {
    float value[4];
    uint32_t offset;
};
CHECK_SIZE(struct GX2UniformInitialValue, 0x14);

struct GX2UniformBlock {
    const char *name;
    uint32_t offset;
    uint32_t size;
};
CHECK_SIZE(struct GX2UniformBlock, 0x0c);

struct GX2AttribVar {
    const char *name;
    uint32_t type;
    uint32_t count;
    uint32_t location;
};
CHECK_SIZE(struct GX2AttribVar, 0x10);

struct GX2SamplerVar {
    const char *name;
    uint32_t type;
    uint32_t location;
};
CHECK_SIZE(struct GX2SamplerVar, 0x0C);

struct GX2LoopVar {
    uint32_t offset;
    uint32_t value;
};
CHECK_SIZE(struct GX2LoopVar, 0x08);

struct GX2RBuffer {
    uint32_t flags;
    uint32_t elem_size;
    uint32_t elem_count;
    void *buffer;
};
CHECK_SIZE(struct GX2RBuffer, 0x10);

struct GX2VertexShader {
    uint32_t sq_pgm_resources_vs;
    uint32_t vgt_primitiveid_en;
    uint32_t spi_vs_out_config;
    uint32_t num_spi_vs_out_id;
    uint32_t spi_vs_out_id[10];
    uint32_t pa_cl_vs_out_cntl;
    uint32_t sq_vtx_semantic_clear;
    uint32_t num_sq_vtx_semantic;
    uint32_t sq_vtx_semantic[32];
    uint32_t vgt_strmout_buffer_en;
    uint32_t vgt_vertex_reuse_block_cntl;
    uint32_t vgt_hos_reuse_depth;

    uint32_t shader_size;
    uint8_t *shader_ptr;
    uint32_t mode;

    struct {
        uint32_t count;
        struct GX2UniformBlock *pointer;
    } uniform_blocks;
    struct {
        uint32_t count;
        struct GX2UniformVar *pointer;
    } uniform_vars;
    struct {
        uint32_t count;
        struct GX2UniformInitialValue *pointer;
    } initial_values;
    struct {
        uint32_t count;
        struct GX2LoopVar *pointer;
    } loop_vars;
    struct {
        uint32_t count;
        struct GX2SamplerVar *pointer;
    } sampler_vars;
    struct {
        uint32_t count;
        struct GX2AttribVar *pointer;
    } attrib_vars;
    uint32_t ring_item_size;
    uint32_t has_stream_out;
    uint32_t stream_out_stride[4];
    struct GX2RBuffer gx2rdata;
};
CHECK_SIZE(struct GX2VertexShader, 0x134);

struct GX2PixelShader {
    uint32_t sq_pgm_resources_ps;
    uint32_t sq_pgm_exports_ps;
    uint32_t spi_ps_in_control_0;
    uint32_t spi_ps_in_control_1;
    uint32_t num_spi_ps_input_cntl;
    uint32_t spi_ps_input_cntl[32];
    uint32_t cb_shader_mask;
    uint32_t cb_shader_control;
    uint32_t db_shader_control;
    uint32_t spi_input_z;

    uint32_t shader_size;
    uint8_t *shader_ptr;
    uint32_t mode;
    struct {
        uint32_t count;
        struct GX2UniformBlock *pointer;
    } uniform_blocks;
    struct {
        uint32_t count;
        struct GX2UniformVar *pointer;
    } uniform_vars;
    struct {
        uint32_t count;
        struct GX2UniformInitialValue *pointer;
    } initial_values;
    struct {
        uint32_t count;
        struct GX2LoopVar *pointer;
    } loop_vars;
    struct {
        uint32_t count;
        struct GX2SamplerVar *pointer;
    } sampler_vars;
    struct GX2RBuffer gx2rdata;
};
CHECK_SIZE(struct GX2PixelShader, 0xe8);

struct GX2Sampler {
    struct {
        uint32_t word0;
        uint32_t word1;
        uint32_t word2;
    } regs;
};
CHECK_SIZE(struct GX2Sampler, 0xc);

struct GX2ContextState {
    uint32_t x[0xa100 / 4];
} __attribute__((aligned(0x100)));
CHECK_SIZE(struct GX2ContextState, 0xa100);

struct GX2ViewportReg {
    float pa_cl_vport_xscale;
    float pa_cl_vport_xoffset;
    float pa_cl_vport_yscale;
    float pa_cl_vport_yoffset;
    float pa_cl_vport_zscale;
    float pa_cl_vport_zoffset;
    float pa_cl_gb_vert_clip_adj;
    float pa_cl_gb_vert_disc_adj;
    float pa_cl_gb_horz_clip_adj;
    float pa_cl_gb_horz_disc_adj;
    float pa_sc_vport_zmin;
    float pa_sc_vport_zmax;
};
CHECK_SIZE(struct GX2ViewportReg, 0x30);

extern void GX2CopyColorBufferToScanBuffer(struct GX2ColorBuffer *buffer, int target);

extern void GX2SetViewport(float, float, float, float, float, float);
extern void GX2SetViewportReg(struct GX2ViewportReg *);
extern void GX2SetScissor(uint32_t, uint32_t, uint32_t, uint32_t);
extern void GX2SetDRCScale(uint32_t, uint32_t);
extern uint32_t GX2CalcFetchShaderSizeEx(uint32_t, uint32_t, uint32_t);
extern void GX2InitFetchShaderEx(struct GX2FetchShader *, void *, uint32_t,
                                 struct GX2AttribStream *, uint32_t, uint32_t);
extern void GX2SetFetchShader(struct GX2FetchShader *);
extern void GX2SetVertexShader(struct GX2VertexShader *);
extern void GX2SetPixelShader(struct GX2PixelShader *);
extern void GX2SetPixelSampler(struct GX2Sampler *, uint32_t);
extern void GX2SetPixelTexture(struct GX2Texture *, uint32_t);
extern bool GX2RCreateBuffer(struct GX2RBuffer *);
extern void GX2RDestroyBufferEx(struct GX2RBuffer *, int);
extern void *GX2RLockBufferEx(struct GX2RBuffer *, int);
extern void GX2RUnlockBufferEx(struct GX2RBuffer *, int);
extern bool GX2RCreateSurface(struct GX2Surface *, uint32_t);
extern void *GX2RLockSurfaceEx(struct GX2Surface *, int, int);
extern void GX2RUnlockSurfaceEx(struct GX2Surface *, int, int);
extern void GX2RDestroySurfaceEx(struct GX2Surface *, uint32_t);
extern void GX2SetupContextStateEx(struct GX2ContextState *, int);
extern void GX2SetContextState(struct GX2ContextState *);
extern void GX2SetDefaultState(void);
extern void GX2SetColorBuffer(struct GX2ColorBuffer *, int);
extern void GX2SetDepthBuffer(struct GX2DepthBuffer *);
extern void GX2InitColorBufferRegs(struct GX2ColorBuffer *);
extern void GX2InitDepthBufferRegs(struct GX2DepthBuffer *);
extern void GX2InitTextureRegs(struct GX2Texture *);
extern void GX2SetBlendControl(int, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                               uint32_t, uint32_t);
extern void GX2SetBlendConstantColor(float, float, float, float);
extern void GX2SetColorControl(uint32_t, uint8_t, bool, bool);
extern void GX2SetDepthOnlyControl(bool, bool, uint32_t);
extern void GX2SetCullOnlyControl(uint32_t, bool, bool);
extern void GX2SetAlphaTest(bool, uint32_t, float);
extern void GX2InitSampler(struct GX2Sampler *sampler, uint32_t, uint32_t);
extern void GX2InitSamplerBorderType(struct GX2Sampler *sampler, uint32_t);
extern void GX2InitSamplerLOD(struct GX2Sampler *sampler, float, float, float);
extern void GX2DrawEx(uint32_t, uint32_t, uint32_t, uint32_t);
extern void GX2DrawIndexedEx(uint32_t, uint32_t, uint32_t, void *, uint32_t, uint32_t);
extern void GX2DrawIndexedEx2(uint32_t, uint32_t, uint32_t, void *, uint32_t, uint32_t, uint32_t);
extern void GX2RSetAttributeBuffer(struct GX2RBuffer *buffer, uint32_t, uint32_t,
                                   uint32_t);
extern void GX2RSetAllocator(void *(*)(uint32_t, size_t, uint32_t),
                             void (*)(uint32_t, void *));
extern void GX2Init(uint32_t *);
extern void GX2SetSwapInterval(uint32_t);
extern void GX2SetTVEnable(bool);
extern void GX2SetDRVEnable(bool);
extern void GX2Flush(void);
extern void GX2ClearColor(struct GX2ColorBuffer *, float, float, float, float);
extern void GX2ClearDepthStencilEx(struct GX2DepthBuffer *, float, uint8_t, uint32_t);
extern bool GX2DrawDone(void);
extern void GX2Invalidate(uint32_t, void *, size_t);
extern void GX2PrintGPUStatus();
extern void GX2SetTargetChannelMasks(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                                     uint32_t, uint32_t, uint32_t);
extern void GX2QueryBegin(uint32_t type, struct GX2QueryData *data);
extern void GX2QueryEnd(uint32_t type, struct GX2QueryData *data);
extern bool GX2QueryGetOcclusionResult(struct GX2QueryData *data, uint64_t *result);
extern void GX2SetRasterizerClipControl(bool, bool);
extern void GX2SetShaderModeEx(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t,
                               uint32_t);
extern bool GX2GetCurrentDisplayList(void **, uint32_t *);
extern void GX2SetSemaphore(uint64_t *, uint32_t);
extern void GX2CopySurface(struct GX2Surface *, uint32_t, uint32_t, struct GX2Surface *,
                           uint32_t, uint32_t);
extern void GX2BeginDisplayListEx(void *, uint32_t, uint32_t);
extern uint32_t GX2EndDisplayList(void *);
extern void GX2CallDisplayList(void *, uint32_t);
extern void GX2DirectCallDisplayList(void *, uint32_t);
extern void GX2CopyDisplayList(void *, uint32_t);



// nlibcurl = curl 7.21.7
typedef struct _CURL CURL;

extern int curl_easy_setopt(CURL *curl, int option, ...);
extern int curl_easy_perform(CURL *curl);
#define CURLOPTTYPE_LONG          0
#define CURLOPTTYPE_OBJECTPOINT   10000
#define CURLOPTTYPE_FUNCTIONPOINT 20000
#define CURLOPTTYPE_OFF_T         30000
#define CURLOPT_URL (CURLOPTTYPE_OBJECTPOINT + 2)

struct boss_Task {
    char _0[8];
    char name[8];
};

// using #define instead of asm because... argh, gcc
// nn::boss::Task::Initialize(char const *, unsigned int)
#define boss_Task_Initialize Initialize__Q3_2nn4boss4TaskFPCcUi
extern int boss_Task_Initialize(boss_Task *, const char *, unsigned int);
// nn::boss::Task::Finalize(void)
#define boss_Task_Finalize Finalize__Q3_2nn4boss4TaskFv
extern void boss_Task_Finalize(boss_Task *);

int vsnprintf(char *buf, size_t len, const char *fmt, va_list ap);

void *memchr(const void *_s, int c, size_t n);
int memcmp(const void *_s1, const void *_s2, size_t n);
#define memcmp(s1, s2, n) __builtin_memcmp(s1, s2, n)
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
char *stpcpy(char *s, const char *src);
int strncmp(const char *s1, const char *s2, size_t n);
int strcmp(const char *s1, const char *s2);

extern void *memset(void *ptr, int c, size_t len);
#define memset(ptr, c, len) __builtin_memset(ptr, c, len)
extern void *memclr(void *ptr, size_t len);
extern void *memcpy(void *dst, const void *src, size_t len);
#define memcpy(dst, src, len) __builtin_memcpy(dst, src, len)
extern void *memmove(void *dst, const void *src, size_t len);
#define memmove(dst, src, len) __builtin_memmove(dst, src, len)

int usleep(uint32_t us);

#define assert(x) ((void)0)

extern char self_elf_start[], data_start[];

} // extern "C"

#else // DUMMY

#define memclr(ptr, len) memset(ptr, 0, len)
#define MEMAllocFromDefaultHeap malloc
static inline void *
MEMAllocFromDefaultHeapEx(size_t size, int align) {
    void *ret;
    if (posix_memalign(&ret, (size_t)align, size))
        return NULL;
    return ret;
}
#define MEMFreeToDefaultHeap free
#define socketlasterr() errno
#define socketclose close
static inline void
OSFatal(const char *msg) {
    fprintf(stderr, "fatal: %s\n", msg);
    exit(1);
}

struct OSEvent {
    manually_construct<std::mutex> mutex;
    manually_construct<std::condition_variable> cond;
    bool val;
    bool autoreset;
};

static inline void
OSInitEvent(OSEvent *event, bool initial, bool autoreset) {
    event->val = initial;
    event->autoreset = autoreset;
    new (&*event->mutex) std::mutex();
    new (&*event->cond) std::condition_variable();
}
static inline void
OSSignalEvent(OSEvent *event) {
    std::unique_lock<std::mutex> _ul(*event->mutex);
    event->val = true;
    event->cond->notify_one();
}
static inline void
OSWaitEvent(OSEvent *event) {
    std::unique_lock<std::mutex> ul(*event->mutex);
    while (!event->val)
        event->cond->wait(ul);
    if (event->autoreset)
        event->val = false;
}

typedef manually_construct<std::mutex> OSMutex;
static inline void
OSInitMutex(OSMutex *mutex) {
    new (&**mutex) std::mutex();
}
static inline void
OSLockMutex(OSMutex *mutex) {
    (*mutex)->lock();
}
static inline void
OSUnlockMutex(OSMutex *mutex) {
    (*mutex)->unlock();
}

typedef manually_construct<std::condition_variable> OSCond;
static inline void
OSInitCond(OSCond *cond) {
    new (&**cond) std::condition_variable();
}
static inline void
OSWaitCond(OSCond *cond, OSMutex *mutex) {
    std::unique_lock<std::mutex> ul(**mutex, std::adopt_lock);
    (*cond)->wait(ul);
}
static inline void
OSSignalCond(OSCond *cond) {
    (*cond)->notify_one();
}

using std::atomic;
using std::memory_order_acquire, std::memory_order_release, std::memory_order_relaxed, std::memory_order_seq_cst;
using std::atomic_store_explicit, std::atomic_thread_fence, std::atomic_compare_exchange_strong;
static inline bool
OSCompareAndSwapAtomic(atomic<uint32_t> *v, uint32_t old, uint32_t nu) {
    return std::atomic_compare_exchange_strong(v, &old, nu);
}
static inline void
OSMemoryBarrier(void) {
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

static inline uint32_t
OSAddAtomic(atomic<uint32_t> *v, uint32_t add) {
    return std::atomic_fetch_add_explicit(v, add, memory_order_seq_cst);
}

__attribute__((format(printf, 3, 4))) static inline int
__os_snprintf(char *buf, size_t len, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, len, fmt, ap);
    va_end(ap);
    return ret;
}

struct OSThread {
    manually_construct<std::thread> stdthread;
    bool started;
    int arg;
    void *arg2;
    int ret;
    const char *name;
    int (*entry)(int, void *);
    thread_deallocator_cb deallocator;
};

extern thread_local OSThread *cur_osthread; // defined in misc.cpp
static inline OSThread *
OSGetCurrentThread(void) {
    OSThread *ret = cur_osthread;
    assert(ret);
    return ret;
}
static inline int
OSCreateThread(OSThread *thread, void *entry, int arg, void *arg2, void *stack,
               size_t stack_size, int prio, int attr) {
    thread->started = false;
    thread->arg = arg;
    thread->arg2 = arg2;
    thread->name = NULL;
    thread->entry = (int (*)(int, void *))entry;
    thread->deallocator = NULL;
    return 1;
}
static inline thread_deallocator_cb
OSSetThreadDeallocator(OSThread *thread,
                       thread_deallocator_cb cb) {
    thread_deallocator_cb old = thread->deallocator;
    thread->deallocator = cb;
    return old;
}
static inline void
OSSetThreadName(OSThread *thread, const char *name) {
    thread->name = name;
}
static inline bool
OSJoinThread(OSThread *thread, int *retp) {
    assert(thread->started);
    thread->stdthread->join();
    *retp = thread->ret;
    thread->stdthread->~thread();
    return true;
}
static inline int
OSResumeThread(OSThread *thread) {
    assert(!thread->started);
    thread->started = true;
    new (&*thread->stdthread) std::thread([=]() {
        cur_osthread = thread;
        const char *name = thread->name;
        if (name) {
            #ifdef __APPLE__
                pthread_setname_np(name);
            #else
                pthread_setname_np(pthread_self(), name);
            #endif
        }
        int ret = thread->entry(thread->arg, thread->arg2);
        thread->ret = ret;
        if (thread->deallocator)
            thread->deallocator(thread, (void *)0xdeadbeef);
    });
    return 1;
}

static inline int
OSDynLoad_GetNumberOfRPLs(void) {
    return 1;
}
static inline int
OSDynLoad_GetRPLInfo(int first, int count, struct rpl_info *infos) {
    for (int i = 0; i < count; i++) {
        infos[i] = (struct rpl_info){
            .name = "dummy.rpx",
// clang-format off
            .text_addr   = 0x10001000, .text_slide   = 0x1000, .text_size   = 0x1000,
            .data_addr   = 0x20001000, .data_slide   = 0x1000, .data_size   = 0x1000,
            .rodata_addr = 0x30001000, .rodata_slide = 0x1000, .rodata_size = 0x1000,
// clang-format on
        };
    }
    return 1;
}

#endif
struct atomic_u32 {
    atomic<uint32_t> val;
};
static inline void
store_release_atomic_u32(struct atomic_u32 *p, uint32_t val) {
    __atomic_store((uint32_t *)&p->val, &val, __ATOMIC_RELEASE);
}
static inline uint32_t
load_acquire_atomic_u32(const struct atomic_u32 *p) {
    uint32_t ret;
    __atomic_load((uint32_t *)&p->val, &ret, __ATOMIC_ACQUIRE);
    return ret;
}

// XXX get rid of this
static inline atomic<uint32_t> *_as_atomic_u32(struct atomic_u32 *p) { return (atomic<uint32_t> *)p; }
static inline atomic<uint32_t> *_as_atomic_u32(atomic<uint32_t> *p) { return p; }
#define OSCompareAndSwapAtomic(v, old, nu) \
    OSCompareAndSwapAtomic(_as_atomic_u32(v), old, nu)
#define OSAddAtomic(v, add) \
    OSAddAtomic(_as_atomic_u32(v), add)
