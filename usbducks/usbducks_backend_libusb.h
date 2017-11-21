#include "decls.h"
#include "logging.h"

#include <libusb-1.0/libusb.h>
#include <pthread.h>
#include <sys/random.h>
#include <time.h>
#include <unistd.h>

struct usbducks_backend {
    pthread_mutex_t mutex;
    libusb_context *ctx;
    libusb_device_handle *handle;
};

struct usbducks_usbtransfer_backend {
    struct libusb_transfer *transfer GUARD;
};

struct usbducks_backend_intf_list {
    libusb_device **device_list;
    ssize_t device_count;
    size_t device_i;
    size_t intf_count;
    size_t intf_i;
    struct libusb_device_descriptor dev_desc;
    struct libusb_config_descriptor *cfg_desc;
    struct libusb_device *dev;
    struct libusb_device_handle *handle;
    char bus_port[16];
};

typedef struct libusb_interface_descriptor usbducks_backend_intf_desc_t;
typedef struct libusb_endpoint_descriptor usbducks_backend_ep_desc_t;

uint32_t usbducks_random_u32(void);
