#include "decls.h"

#define USBDUCKS_BACKEND_CLIENT_BUF_SIZE 8192 // size must be >= 4991..?

struct usbducks_backend {
    OSMutex mutex;
    struct uhs_client uhs_client;
    uint32_t if_handle;
    OSEvent wakeup_event;
    OSThread main_loop_thread;
    char main_loop_thread_stack[0x4000];
    char client_buf[USBDUCKS_BACKEND_CLIENT_BUF_SIZE] __attribute__((aligned(0x40)));
    struct {
        char buf[USBDUCKS_USBTRANSFER_SEND_BUF_CAP] __attribute__((aligned(0x100)));
    } send_bufs[USBDUCKS_NUM_SEND_USBTRANSFERS];
    struct {
        char buf[USBDUCKS_USBTRANSFER_RECV_BUF_CAP] __attribute__((aligned(0x100)));
    } recv_bufs[USBDUCKS_NUM_RECV_USBTRANSFERS];
};

struct usbducks_usbtransfer_backend {
    int ret;
};

struct usbducks_backend_intf_list {
    struct UhsInterfaceProfile *profiles;
    size_t num_profiles;
    size_t profiles_i;
};

typedef struct UhsInterfaceDescriptor usbducks_backend_intf_desc_t;
typedef struct UhsEndpointDescriptor usbducks_backend_ep_desc_t;

static inline uint32_t
usbducks_random_u32(void) {
    return (uint32_t)OSGetTime(); // *shrug*
}

struct usbducks;
void usbducks_backend_ensure_lock_not_owned(struct usbducks *ud);
