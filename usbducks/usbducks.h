#pragma once
#include "types.h"

constexpr int USBDUCKS_VERBOSE = DUMMY ? 2 : 2;

constexpr uint16_t USBDUCKS_VID_TO_USE = 0x067b;
constexpr uint16_t USBDUCKS_PID_TO_USE = 0x27a1;

// Using a non-huge timeout causes us to silently lose data on macOS.
// On Wii U, the async API doesn't actually let us specify a timeout without
// using IOS_IoctlvAsync directly, so both values are ignored.
constexpr int USBDUCKS_SEND_TIMEOUT_MS = 500;
constexpr int USBDUCKS_RECV_TIMEOUT_MS = 999999;

constexpr int USBDUCKS_SEND_EP_ADDR = 0x08;
constexpr int USBDUCKS_RECV_EP_ADDR = 0x89;

constexpr size_t USBDUCKS_NUM_SEND_USBTRANSFERS = 2;
constexpr size_t USBDUCKS_NUM_RECV_USBTRANSFERS = 3;
constexpr size_t USBDUCKS_USBTRANSFER_SEND_BUF_CAP = 1024 * 4;
constexpr size_t USBDUCKS_USBTRANSFER_RECV_BUF_CAP = 1 * USBDUCKS_USBTRANSFER_SEND_BUF_CAP;

constexpr size_t USBDUCKS_MAX_EP_DESCS = 8;
constexpr size_t USBDUCKS_MAX_CONNECTIONS_PER_DIRECTION = 3;
constexpr size_t USBDUCKS_MAX_CONNECTIONS = 2 * USBDUCKS_MAX_CONNECTIONS_PER_DIRECTION;
constexpr size_t USBDUCKS_MAX_LISTENERS = 8;

#if USE_THREAD_SAFETY_ANALYSIS
static struct __attribute__((capability("mutex"))) {
} _ud_mutex;
#endif
#define GUARD GUARDED_BY(_ud_mutex)
#define LOCKED REQUIRES(_ud_mutex)

#if DUMMY
#include "usbducks_backend_libusb.h"
#else
#include "usbducks_backend_wiiu.h"
#endif

struct usbducks_intf_list_entry {
    const usbducks_backend_intf_desc_t *intf_desc;
    const usbducks_backend_ep_desc_t *ep_descs[USBDUCKS_MAX_EP_DESCS];
    char bus_port[40];
};

enum usbducks_direction {
    UDD_SEND = 0,
    UDD_RECV = 1,
};

enum usbducks_transfer_retcode {
    UTR_OK = 0,
    UTR_TIMED_OUT = 1,
    UTR_ERROR = 2,
};

enum usbducks_transfer_state {
    UTS_FREE = 0,
    UTS_PENDING = 1,
    UTS_FINISHED = 2,
    UTS_PARTIAL = 3, // finished, but buffer is still in use for partial packet
};

struct usbducks_usbtransfer {
    struct usbducks *ud;
    struct usbducks_usbtransfer_backend be;
    enum usbducks_direction direction GUARD;
    uint8_t ep GUARD;
    uint8_t next_pending_transfer_idx GUARD;
    void *buf;
    size_t buf_cap GUARD;
    size_t buf_len GUARD;
    size_t buf_off GUARD; // only for partial
    uint64_t start_time GUARD; // also just for stats
    size_t actual_len;
    atomic<enum usbducks_transfer_state> state;
    enum usbducks_transfer_retcode rc;
};

struct usbducks_usbtransfer_list {
    uint8_t total_count;
    uint8_t pending_count GUARD;
    uint8_t first_pending_transfer_idx GUARD;
    uint8_t last_pending_transfer_idx GUARD;
    uint8_t partial_transfer_idx GUARD; // only for recv
    size_t bytes_xferred GUARD; // just for stats
    struct usbducks_usbtransfer *transfers;
};

// TODO handle freeing properly
struct usbducks_connection {
    struct usbducks *ud;
    bool active GUARD;
    uint16_t port GUARD;
    bool told_remote GUARD;
    size_t clear_to_send GUARD;
    size_t clear_to_recv GUARD;
    uint8_t conn_id;
    uint32_t epoch;
    const void *pending_send_buf GUARD;
    size_t pending_send_len GUARD;
    uint64_t pending_send_time GUARD;

    void *user GUARD;
    void *user2 GUARD;

    void (*on_send_complete)(struct usbducks_connection *conn, size_t actual, uint64_t time) GUARD;
    void (*on_clear_to_send)(struct usbducks_connection *conn) GUARD;
    void (*on_receive)(struct usbducks_connection *conn, const void *buf,
                       size_t len) GUARD;
    void (*on_disconnect)(struct usbducks_connection *conn) GUARD;

    size_t len_to_send_tmp;
};

struct usbducks_listener {
    bool active GUARD;
    bool told_remote GUARD;
    uint16_t port;
    void *user GUARD;
    void (*on_incoming_conn)(struct usbducks_listener *listener,
                             struct usbducks_connection *conn) GUARD;
};

enum usbducks_sync_state {
    USS_SYNC,
    USS_PING_PONG,
    USS_NORMAL,
};

struct usbducks {
    bool did_init;
    struct usbducks_backend be;
    struct usbducks_usbtransfer_list transfer_lists[2] GUARD; // by direction
    struct usbducks_usbtransfer send_transfers[USBDUCKS_NUM_SEND_USBTRANSFERS] GUARD;
    struct usbducks_usbtransfer recv_transfers[USBDUCKS_NUM_RECV_USBTRANSFERS] GUARD;

    enum usbducks_sync_state sync_state GUARD;
    size_t sync_muggle_len GUARD;
    size_t sync_off_into_magic GUARD;
    bool skip_next_magic GUARD;

    uint32_t my_client_id GUARD;
    uint32_t peer_client_id GUARD;
    uint32_t epoch GUARD;
    bool have_peer_client_id GUARD;
    uint8_t next_seq_num GUARD;
    bool have_peer_last_seq_num GUARD;
    uint8_t peer_last_seq_num GUARD;
    bool has_sent_successfully GUARD;
    struct usbducks_connection connections[USBDUCKS_MAX_CONNECTIONS] GUARD;
    struct usbducks_listener listeners[USBDUCKS_MAX_LISTENERS] GUARD;

    size_t need_pong_len GUARD;
    uint8_t need_pong_seq_num GUARD;
    uint64_t need_pong_ping_time GUARD;

    uint64_t last_outgoing_pingpong_time GUARD;

    bool have_last_outgoing_ping GUARD;
    uint8_t last_outgoing_ping_seq_num GUARD;
    uint64_t last_outgoing_ping_time GUARD;

    void *user GUARD;
    void (*on_remote_listen)(struct usbducks *ud, uint16_t port) GUARD;
};

struct usbducks_pkthdr {
    uint64_t magic;
    uint8_t version;
    uint8_t pkt_type;
    uint8_t conn_id;
    uint8_t seq_num;
    uint32_t client_id;
    uint32_t body_len; // not including header
    char body[];
} __attribute__((packed));

struct usbducks_pkttrailer {
    uint32_t client_id;
    uint8_t seq_num;
} __attribute__((packed));

struct usbducks_pong_data {
    uint8_t seq_num;
    uint32_t client_id;
} __attribute__((packed));

static const uint64_t USBDUCKS_MAGIC = 0x5553426475636b73; // USBducks
static const uint8_t USBDUCKS_VERSION = 1;
enum : uint8_t {
    USBDUCKS_PKTTYPE_PING = 0,
    USBDUCKS_PKTTYPE_PONG = 1,
    USBDUCKS_PKTTYPE_IGNORE = 2,
    USBDUCKS_PKTTYPE_CONNECT = 3,
    USBDUCKS_PKTTYPE_CLEAR_TO_SEND = 4,
    USBDUCKS_PKTTYPE_DATA = 5,
    USBDUCKS_PKTTYPE_DISCONNECT = 6,
    USBDUCKS_PKTTYPE_I_AM_LISTENING = 7,
};

constexpr size_t USBDUCKS_PING_BODY_LEN =
    USBDUCKS_USBTRANSFER_SEND_BUF_CAP
    - sizeof(struct usbducks_pkthdr)
    - sizeof(struct usbducks_pkttrailer);

void usbducks_check_finished_usbtransfers(struct usbducks *ud) LOCKED;
void usbducks_reset_peer(struct usbducks *ud) LOCKED;
void usbducks_init(struct usbducks *ud);
void usbducks_start(struct usbducks *ud) LOCKED;

struct usbducks_connection *usbducks_connect(struct usbducks *ud, uint16_t port) LOCKED;
struct usbducks_listener *usbducks_listen(struct usbducks *ud, uint16_t port) LOCKED;
void usbducks_send_async(struct usbducks_connection *conn, const void *buf,
                         size_t len) LOCKED;
void usbducks_clear_to_recv(struct usbducks_connection *conn, size_t len) LOCKED;
void usbducks_disconnect(struct usbducks_connection *conn) LOCKED;

void usbducks_start_transfers_if_necessary(struct usbducks *ud) LOCKED;
uint64_t usbducks_get_wait_timeout_us(struct usbducks *ud) LOCKED;

// backend
void usbducks_backend_intf_list_init(struct usbducks *ud,
                                     struct usbducks_backend_intf_list *il);
bool usbducks_backend_intf_list_next(struct usbducks *ud,
                                     struct usbducks_backend_intf_list *il,
                                     struct usbducks_intf_list_entry *ile);
bool usbducks_backend_intf_list_open(struct usbducks *ud,
                                     struct usbducks_backend_intf_list *il);
void usbducks_backend_intf_list_fini(struct usbducks *ud,
                                     struct usbducks_backend_intf_list *il);
void usbducks_backend_init(struct usbducks *ud);
void usbducks_backend_usbtransfer_init(struct usbducks_usbtransfer *ut) LOCKED;
void usbducks_backend_usbtransfer_start(struct usbducks_usbtransfer *ut) LOCKED;
size_t usbducks_backend_control_req(struct usbducks *ud, uint8_t bmRequestType,
                                    uint8_t bRequest, uint16_t wValue, uint16_t wIndex,
                                    unsigned char *data, uint16_t wLength,
                                    uint32_t timeout_ms);
void usbducks_backend_start_main_loop_thread(struct usbducks *ud);

void usbducks_backend_lock(struct usbducks *ud)
    ACQUIRE(_ud_mutex) NO_THREAD_SAFETY_ANALYSIS;
void usbducks_backend_unlock(struct usbducks *ud)
    RELEASE(_ud_mutex) NO_THREAD_SAFETY_ANALYSIS;
