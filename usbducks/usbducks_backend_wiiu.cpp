#include "logging.h"
#include "decls.h"
#include "misc.h"
#include "usbducks.h"

#undef THIS_C_FILE_IS_USBDUCKS_RELATED
#define THIS_C_FILE_IS_USBDUCKS_RELATED 1
static_assert(LOG_FLAGS_TO_USE == LOG_NOUSBDUCKS); // this was messed up before

void
usbducks_backend_init(struct usbducks *ud) {
    ensure(((uintptr_t)ud & 7) == 0);

    OSInitMutex(&ud->be.mutex);
    memset(&ud->be.uhs_client, 0, sizeof(ud->be.uhs_client));
    OSInitEvent(&ud->be.wakeup_event, false, true);

    struct uhs_client_opts opts = {
        .which = 0,
        .buf = ud->be.client_buf,
        .buf_size = USBDUCKS_BACKEND_CLIENT_BUF_SIZE,
    };
    ensure_eq(0, UhsClientOpen(&ud->be.uhs_client, &opts));
    log("did open\n");
}

void
usbducks_backend_intf_list_init(struct usbducks *ud,
                                struct usbducks_backend_intf_list *il) {
    size_t cap = 8;
    size_t bytes = cap * sizeof(*il->profiles);
    il->profiles = (struct UhsInterfaceProfile *)MEMAllocFromDefaultHeapEx(bytes, 0x100);
    ensure(il->profiles);

    struct UhsInterfaceFilter filter;
    memset(&filter, 0, sizeof(filter));
    filter.match_params = MATCH_DEV_VID | MATCH_DEV_PID;
    filter.vid = USBDUCKS_VID_TO_USE;
    filter.pid = USBDUCKS_PID_TO_USE;

    memset(il->profiles, 0x88, bytes);
    log("UhsQueryInterfaces=%p\n", UhsQueryInterfaces);
    int ret = UhsQueryInterfaces(&ud->be.uhs_client, &filter, il->profiles, cap);
    ensure_op(ret, >=, 0);
    log("ret=%d\n", ret);
    for (int i = 0; i < ret; i++) {
        log("x[%d] = %x, ifc=%x\n", i, il->profiles[i].if_handle,
            il->profiles[i].intf_desc.bInterfaceClass);
    }
    il->num_profiles = (size_t)ret;
    il->profiles_i = 0;
}

bool
usbducks_backend_intf_list_next(struct usbducks *ud,
                                struct usbducks_backend_intf_list *il,
                                struct usbducks_intf_list_entry *ile) {
    if (il->profiles_i >= il->num_profiles)
        return false;
    struct UhsInterfaceProfile *profile = &il->profiles[il->profiles_i++];
    ile->intf_desc = &profile->intf_desc;
    size_t j = 0;
    for (size_t i = 0; i < 32 && j < USBDUCKS_MAX_EP_DESCS; i++) {
        if (profile->endpoints[i].bLength)
            ile->ep_descs[j++] = &profile->endpoints[i];
    }
    size_t first_port_idx = 0;
    while (first_port_idx < 7 && profile->ports[first_port_idx] == 0xffff)
        first_port_idx++;
    char *p = ile->bus_port, *end = p + sizeof(ile->bus_port);
    usprintf(&p, end, "%02x", profile->ports[first_port_idx]);
    for (size_t i = first_port_idx; i < 8; i++)
        usprintf(&p, end, ".%02x", profile->ports[i]);
    return true;
}

static void
intf_callback(void *user, uint32_t if_handle, uint32_t event) {
    struct usbducks *ud = (struct usbducks *)user;
    while (1)
        log("!!intf_callback: user=%p if_handle=%u event=%u\n", user, if_handle, event);
    (void)ud;
}

bool
usbducks_backend_intf_list_open(struct usbducks *ud,
                                struct usbducks_backend_intf_list *il) {
    uint32_t if_handle = il->profiles[il->profiles_i - 1].if_handle;
    ud->be.if_handle = if_handle;
    int ret = UhsAcquireInterface(&ud->be.uhs_client, if_handle, ud,
                                  intf_callback);
    if (ret) {
        log("UhsAcquireInterface(0x%x) => %d\n", if_handle, ret);
        return false;
    }
    uint32_t mask = (1 << (USBDUCKS_SEND_EP_ADDR & 0xf))
        | (1 << ((USBDUCKS_RECV_EP_ADDR & 0xf) | 0x10));
    ret = UhsAdministerEndpoint(&ud->be.uhs_client, if_handle, 1, mask,
                                USBDUCKS_NUM_SEND_USBTRANSFERS
                                    + USBDUCKS_NUM_RECV_USBTRANSFERS,
                                USBDUCKS_USBTRANSFER_RECV_BUF_CAP);

    if (ret) {
        log("UhsAdministerEndpoint => %d\n", ret);
        return false;
    }
    return true;
}

void
usbducks_backend_intf_list_fini(struct usbducks *ud,
                                struct usbducks_backend_intf_list *il) {
    MEMFreeToDefaultHeap(il->profiles);
}

void
usbducks_backend_usbtransfer_init(struct usbducks_usbtransfer *ut) {
    struct usbducks *ud = ut->ud;
    size_t size;
    char *ptr;
    switch (ut->direction) {
    case UDD_SEND: {
        ssize_t i = ut - ud->send_transfers;
        char (*bufp)[USBDUCKS_USBTRANSFER_SEND_BUF_CAP] = &ud->be.send_bufs[i].buf;
        size = sizeof(*bufp);
        ptr = *bufp;
        break;
    }
    case UDD_RECV: {
        ssize_t i = ut - ud->recv_transfers;
        char (*bufp)[USBDUCKS_USBTRANSFER_RECV_BUF_CAP] = &ud->be.recv_bufs[i].buf;
        size = sizeof(*bufp);
        ptr = *bufp;
        break;
    }
    default:
        ensure(false);
    }
    ensure(size == ut->buf_cap);
    ensure(!((uintptr_t)ptr & 0xff));
    ut->buf = ptr;
}

static void
transfer_callback(void *user, int ret, uint32_t if_handle, uint8_t endpoint,
                  int direction, const void *buffer, size_t actual) {
    // This is on a callback thread; need to post elsewhere
    struct usbducks_usbtransfer *ut = (struct usbducks_usbtransfer *)user;
    struct usbducks *ud = ut->ud;
    ensure(buffer == ut->buf);
    ut->actual_len = actual;
    if (ret >= 0)
        ut->rc = UTR_OK;
    else if (ret == -0x21003b)
        ut->rc = UTR_TIMED_OUT;
    else
        ut->rc = UTR_ERROR;
    ut->be.ret = ret;
    atomic_store_explicit(&ut->state, UTS_FINISHED, memory_order_release);
    OSSignalEvent(&ud->be.wakeup_event);
}

void
usbducks_backend_usbtransfer_start(struct usbducks_usbtransfer *ut) {
    struct usbducks *ud = ut->ud;
    int ret;
    if (USBDUCKS_VERBOSE >= 2)
        log("UhsSubmitBulkRequestAsync ut=%p handle=%x ep=%x dir=%x =>\n", ut,
            ud->be.if_handle, ut->ep, ut->direction);
    if ((ret = UhsSubmitBulkRequestAsync(
             &ud->be.uhs_client, ud->be.if_handle, ut->ep & 0xf,
             ut->direction == UDD_SEND ? ENDPOINT_TRANSFER_OUT : ENDPOINT_TRANSFER_IN,
             ut->buf, ut->buf_len, ut, transfer_callback))
        != 0)
        panic("UhsSubmitBulkRequestAsync: %d", ret);
    if (USBDUCKS_VERBOSE >= 2)
        log("=> %d\n", ret);
}

size_t
usbducks_backend_control_req(struct usbducks *ud, uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, unsigned char *data,
                             uint16_t wLength, uint32_t timeout_ms) {
    log("UhsSubmitControlRequest(%p, %x) =>\n", data, ud->be.if_handle);
    int ret;
    if ((ret
         = UhsSubmitControlRequest(&ud->be.uhs_client, ud->be.if_handle, data, bRequest,
                                   bmRequestType, wValue, wIndex, wLength, (int)timeout_ms))
        < 0)
        panic("UhsSubmitControlRequest: %d", ret);
    log("=>%d\n", ret);
    return (size_t)ret;
}

static int
usbducks_backend_thread_func(int i, void *xud) {
    log("usbducks_backend_thread_func(%d, %p)\n", i, xud);
    struct usbducks *ud = (struct usbducks *)xud;
    while (1) {
        usbducks_backend_lock(ud);
        for (int direction = UDD_SEND; direction <= UDD_RECV; direction++) {
            struct usbducks_usbtransfer_list *list = &ud->transfer_lists[direction];

            // print this ASAP even if another transfer is ahead in line
            for (size_t i = 0; i < list->total_count; i++) {
                struct usbducks_usbtransfer *ut = &list->transfers[i];
                if (UTS_FINISHED == atomic_load_explicit(&ut->state, memory_order_acquire)
                    && ut->rc == UTR_ERROR && ut->be.ret) {
                    log("transfer %p error code: -%x\n", ut, -ut->be.ret);
                    ut->be.ret = 0;
                }
            }
        }

        usbducks_check_finished_usbtransfers(ud);
        usbducks_start_transfers_if_necessary(ud);
        uint64_t timeout = usbducks_get_wait_timeout_us(ud);
        usbducks_backend_unlock(ud);
        //log("usbducks_backend_thread_func: timeout=%llu\n", timeout);
        OSWaitEventWithTimeout(&ud->be.wakeup_event, timeout * 1000);
    }
}

void
usbducks_backend_start_main_loop_thread(struct usbducks *ud) {
    ensure(OSCreateThread(&ud->be.main_loop_thread, (void *)usbducks_backend_thread_func, 0, ud,
                          ud->be.main_loop_thread_stack
                              + sizeof(ud->be.main_loop_thread_stack),
                          sizeof(ud->be.main_loop_thread_stack),
                          16, // prio
                          8)); // detach
    OSSetThreadName(&ud->be.main_loop_thread, "gdbstub usbducks");
    ensure(OSResumeThread(&ud->be.main_loop_thread));
    usbducks_backend_lock(ud);
    usbducks_start_transfers_if_necessary(ud);
    usbducks_backend_unlock(ud);
}

void
usbducks_backend_lock(struct usbducks *ud) {
    OSLockMutex(&ud->be.mutex);
    ensure(ud->did_init);
}

void
usbducks_backend_unlock(struct usbducks *ud) {
    OSUnlockMutex(&ud->be.mutex);
}

void
usbducks_backend_ensure_lock_not_owned(struct usbducks *ud) {
    if (ud->be.mutex.owner == OSGetCurrentThread())
        panic("usbducks_backend_ensure_lock_not_owned: lock inversion\n");
}
