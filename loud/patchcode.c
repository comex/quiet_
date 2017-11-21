struct ioctlv {
    void *addr;
    unsigned len;
    void *aux;
};
struct m {
    int fd;
    struct {
        struct {
            int domain, type, protocol;
        } socket;
        struct {
            int sock;
            struct {
                short sin_family;
                short sin_port;
                unsigned sin_addr;
                int sin_zero1;
                int sin_zero2;
            } addr;
            unsigned addrlen;
        } connect;
        struct {
            struct {
                int sock;
                int flags;
            } info;
            struct ioctlv ioctlvs[4];
        } send;
    };
    char sbuf[0x5c6] __attribute__((aligned(0x40)));
};

//#define MY_BUF (*(struct m **)0x50820000) // for testing
#define MY_BUF (*(struct m **)0x050BC574) // end of bss
#define IOS_AllocEx ((void *(*)(int, unsigned, unsigned))0x505692C)
#define IOS_Open ((int (*)(const char *, int))0x05056984)
#define IOS_Ioctl ((int (*)(int, int, void *, unsigned, void *, unsigned))0x050569AC)
#define IOS_Ioctlv ((int (*)(int, int, unsigned, unsigned, struct ioctlv *))0x050569B4)

void
syslog_stage2(const char *buf, unsigned len, __attribute__((unused)) int flags) {
    // MY_BUF = (void *)15;
    struct m *m = MY_BUF;
    register int status;
    if (m == 0) {
        m = IOS_AllocEx(0xcaff, sizeof(*m), __alignof(*m));
        status = 1;
        if (!m)
            goto bad;
        for (unsigned i = 0; i < sizeof(*m); i += 4)
            *(int *)((char *)m + i) = 0;
        int fd = IOS_Open("/dev/socket", 0);
        status = 3;
        if (fd < 0)
            goto bad;
        m->fd = fd;
        m->socket.domain = 2;
        m->socket.type = 1;
        m->socket.protocol = 0;
        // socket
        int sock = IOS_Ioctl(fd, 0x11, &m->socket, sizeof(m->socket), 0, 0);
        status = 5;
        if (sock < 0)
            goto bad;
        m->send.info.sock = sock;
        m->connect.sock = sock;
        m->connect.addr.sin_family = 2;
        m->connect.addr.sin_port = 1236;
        m->connect.addr.sin_addr = (192u << 24) | (168u << 16) | (1 << 8) | 131u;
        // m->connect.addr.sin_zero1 = m->connect.addr.sin_zero2 = 0;
        m->connect.addrlen = sizeof(m->connect.addr);
        int cret = IOS_Ioctl(fd, 4, &m->connect, sizeof(m->connect), 0, 0);
        status = 7;
        if (cret < 0)
            goto bad;
        MY_BUF = m;
    } else if ((int)m < 16)
        return;
    while (len > 0) {
        char *sbuf = m->sbuf;
        unsigned i = 0, j = 0;
        while (1) {
            if (j >= sizeof(m->sbuf) - 16)
                break;
            if (i >= len) {
                /*
                sbuf[j++] = '@';
                sbuf[j++] = '\n';
                */
                break;
            }
            char c = buf[i++];
            sbuf[j++] = c;
            if (c == '\r')
                sbuf[j++] = '\n';
        }

        len -= i;
        buf += i;
        // m->send.info.sock (set above)
        // m->send.info.flags = 0;
        m->send.ioctlvs[0].addr = &m->send.info;
        m->send.ioctlvs[0].len = sizeof(m->send.info);
        m->send.ioctlvs[1].addr = sbuf;
        m->send.ioctlvs[1].len = j;

        /*
        m->send.ioctlvs[2].addr = 0;
        m->send.ioctlvs[2].len = 0;
        m->send.ioctlvs[3].addr = 0;
        m->send.ioctlvs[3].len = 0;
        */

        int sret = IOS_Ioctlv(m->fd, 0xe, 4, 0, m->send.ioctlvs);
        status = 9;
        if (sret < (int)i)
            goto bad;
    }
    return;

bad:
    MY_BUF = (void *)status;
}
