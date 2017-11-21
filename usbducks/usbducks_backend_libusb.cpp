#define DUMMY 1
#include "decls.h"
#include "logging.h"
#include "usbducks.h"

void
usbducks_backend_init(struct usbducks *ud) {
    ensure(!pthread_mutex_init(&ud->be.mutex, NULL));

    int ret;
    if ((ret = libusb_init(&ud->be.ctx)) < 0)
        panic("libusb_init: %s", libusb_error_name(ret));
}

void
usbducks_backend_intf_list_init(struct usbducks *ud,
                                struct usbducks_backend_intf_list *il) {
    il->device_count = libusb_get_device_list(ud->be.ctx, &il->device_list);
    if (il->device_count < 0)
        panic("libusb_get_intf_list: %s", libusb_error_name((int)il->device_count));
    il->device_i = 0;
    il->intf_i = 0;
    il->intf_count = 0;
    il->handle = NULL;
    il->cfg_desc = NULL;
}

static void
intf_list_cleanup(struct usbducks_backend_intf_list *il) {
    if (il->cfg_desc) {
        libusb_free_config_descriptor(il->cfg_desc);
        il->cfg_desc = NULL;
    }
    if (il->handle) {
        printf("closing handle\n");
        libusb_close(il->handle);
        il->handle = NULL;
    }
}

bool
usbducks_backend_intf_list_next(struct usbducks *ud,
                                struct usbducks_backend_intf_list *il,
                                struct usbducks_intf_list_entry *ile) {
    int ret;
    while (1) {
        if (il->intf_i >= il->intf_count) {
            intf_list_cleanup(il);
            if (il->device_i >= il->device_count)
                return false;
            size_t device_i = il->device_i++;
            struct libusb_device *dev = il->device_list[device_i];
            il->dev = dev;
            uint8_t bus = libusb_get_bus_number(dev);
            uint8_t port = libusb_get_port_number(dev);
            snprintf(il->bus_port, sizeof(il->bus_port), "%x:%x", bus, port);
            if ((ret = libusb_get_device_descriptor(dev, &il->dev_desc)) < 0) {
                log("libusb_get_device_descriptor(%s) failed (trying more): %s\n",
                    il->bus_port, libusb_error_name(ret));
                continue;
            }
            if (il->dev_desc.idVendor != USBDUCKS_VID_TO_USE
                || il->dev_desc.idProduct != USBDUCKS_PID_TO_USE)
                continue;
            ensure_eq(il->dev_desc.bNumConfigurations, 1);
            if ((ret = libusb_get_config_descriptor(dev, 0, &il->cfg_desc)) < 0) {
                il->cfg_desc = NULL;
                log("libusb_get_config_descriptor(%s, 0) failed (trying more): %s\n",
                    il->bus_port, libusb_error_name(ret));
                continue;
            }
            il->intf_i = 0;
            il->intf_count = il->cfg_desc->bNumInterfaces;
        }
        size_t intf_i = il->intf_i++;
        if (il->cfg_desc->interface[intf_i].num_altsetting != 1)
            continue;
        ile->intf_desc = &il->cfg_desc->interface[intf_i].altsetting[0];
        for (size_t i = 0; i < ile->intf_desc->bNumEndpoints && i < USBDUCKS_MAX_EP_DESCS;
             i++) {
            ile->ep_descs[i] = &ile->intf_desc->endpoint[i];
        }
        memcpy(ile->bus_port, il->bus_port, 16);
        return true;
    }
}

bool
usbducks_backend_intf_list_open(struct usbducks *ud,
                                struct usbducks_backend_intf_list *il) {
    int ret;
    if ((ret = libusb_open(il->dev, &il->handle)) < 0) {
        il->handle = NULL;
        log("failed to open matching device %s (trying more): %s\n", il->bus_port,
            libusb_error_name(ret));
        return false;
    }

    libusb_set_auto_detach_kernel_driver(il->handle, 1); // might fail
    if ((ret = libusb_set_configuration(il->handle, 0)) < 0) {
        log("libusb_set_configuration(%s, 0) failed (trying more): %s\n", il->bus_port,
            libusb_error_name(ret));
        return false;
    }
    if ((ret = libusb_claim_interface(il->handle, (int)il->intf_i - 1)) < 0) {
        log("libusb_claim_interface(%s, %d) failed (trying more): %s\n", il->bus_port,
            (int)il->intf_i, libusb_error_name(ret));
        return false;
    }

    ud->be.handle = il->handle;
    il->handle = NULL; // don't close it
    return true;
}

void
usbducks_backend_intf_list_fini(struct usbducks *ud,
                                struct usbducks_backend_intf_list *il) {
    intf_list_cleanup(il);
    libusb_free_device_list(il->device_list, 1);
}

void
usbducks_backend_usbtransfer_init(struct usbducks_usbtransfer *ut) {
    struct usbducks *ud = ut->ud;
    ut->buf = libusb_dev_mem_alloc(ud->be.handle, ut->buf_cap);
    if (!ut->buf) {
        ut->buf = malloc(ut->buf_cap);
        ensure(ut->buf);
    }
    ut->be.transfer = libusb_alloc_transfer(0);
    ensure(ut->be.transfer);
}

static void
transfer_cb(struct libusb_transfer *transfer) {
    struct usbducks_usbtransfer *ut = (struct usbducks_usbtransfer *)transfer->user_data;
    struct usbducks *ud = ut->ud;
    ut->actual_len = (size_t)transfer->actual_length;
    switch (transfer->status) {
    case LIBUSB_TRANSFER_COMPLETED:
        ut->rc = UTR_OK;
        break;
    case LIBUSB_TRANSFER_TIMED_OUT:
        ut->rc = UTR_TIMED_OUT;
        break;
    default:
        log("pending transfer %p failed: %s\n", transfer,
            libusb_error_name(transfer->status));
        ut->rc = UTR_ERROR;
        break;
    }
    usbducks_backend_lock(ud);
    atomic_store_explicit(&ut->state, UTS_FINISHED, memory_order_release);
    usbducks_check_finished_usbtransfers(ud);
    usbducks_start_transfers_if_necessary(ut->ud);
    usbducks_backend_unlock(ud);
}

void
usbducks_backend_usbtransfer_start(struct usbducks_usbtransfer *ut) {
    struct usbducks *ud = ut->ud;
    int ret;
    libusb_fill_bulk_transfer(
        ut->be.transfer, ud->be.handle,
        ut->direction == UDD_SEND ? USBDUCKS_SEND_EP_ADDR : USBDUCKS_RECV_EP_ADDR,
        (unsigned char *)ut->buf, (int)ut->buf_len, transfer_cb, ut,
        ut->direction == UDD_SEND ? USBDUCKS_SEND_TIMEOUT_MS : USBDUCKS_RECV_TIMEOUT_MS);
    if ((ret = libusb_submit_transfer(ut->be.transfer)) < 0)
        panic("libusb_submit_transfer(ut=%p): %s", ut, libusb_error_name(ret));
}

size_t
usbducks_backend_control_req(struct usbducks *ud, uint8_t bmRequestType, uint8_t bRequest,
                             uint16_t wValue, uint16_t wIndex, unsigned char *data,
                             uint16_t wLength, uint32_t timeout_ms) {
    int ret;
    if ((ret = libusb_control_transfer(ud->be.handle, bmRequestType, bRequest, wValue,
                                       wIndex, data, wLength, timeout_ms))
        < 0)
        panic("libusb_control_transfer: %s", libusb_error_name(ret));
    return (size_t)ret;
}

static void *
usbducks_backend_thread_func(void *xud) {
    struct usbducks *ud = (struct usbducks *)xud;
    while (1) {
        usbducks_backend_lock(ud);
        usbducks_start_transfers_if_necessary(ud);
        struct timeval tv;
        uint64_t timeout = usbducks_get_wait_timeout_us(ud);
        // log("timeout=%u\n", timeout);
        usbducks_backend_unlock(ud);
        int ret;
        tv.tv_sec = timeout / 1000000;
        tv.tv_usec = timeout % 1000000;
        if ((ret = libusb_handle_events_timeout(ud->be.ctx, &tv)) < 0)
            panic("libusb_handle_events_timeout: %s\n", libusb_error_name(ret));
    }
}

void
usbducks_backend_start_main_loop_thread(struct usbducks *ud) {
    pthread_t pt;
    ensure_errno(!pthread_create(&pt, NULL, usbducks_backend_thread_func, ud));
}

uint32_t
usbducks_random_u32(void) {
    uint32_t ret;
    ensure_errno(!getentropy(&ret, sizeof(ret)));
    return ret;
}

void
usbducks_backend_lock(struct usbducks *ud) {
    ensure(!pthread_mutex_lock(&ud->be.mutex));
}
void
usbducks_backend_unlock(struct usbducks *ud) {
    ensure(!pthread_mutex_unlock(&ud->be.mutex));
}
