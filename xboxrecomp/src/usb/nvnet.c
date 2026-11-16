/* The MCPX Ethernet controller (nForce "nvnet"), for System Link.
 *
 * Registers, descriptor rings and PHY after xemu's hw/xbox/mcpx/nvnet
 * (LGPL, espes / Matt Borgerson): the same register semantics, so the
 * title's own network stack (XNet, linked into the game) drives it as on a
 * console. Frames go out as UDP datagrams on the phone's Wi-Fi: broadcasts
 * to the LAN broadcast address, unicasts to the address the destination MAC
 * was last heard from. Two phones on one network see each other as two
 * consoles on one cable.
 *
 * The interrupt (IRQ 4, vector 4) is delivered from this device's own
 * thread the way the USB controller does it (ohci.c): posted to the
 * one-CPU gate holder when the gate is on.
 *
 * CONKER_NO_NIC=1: the old plain-memory aperture (no network at all).
 * CONKER_MAC=aa:bb:cc:dd:ee:ff: this console's MAC (the app makes one per
 * install); CONKER_NET_PORT: the UDP port (default 39123). */
#include "../platform/mmio_decode.h"
#include "../platform/mmio_decode_a64.h"
#include "../platform/mmio_decode_x86_64.h"
#include "../kernel/xbox_memory_layout.h"
#include "../kernel/kernel.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>
#if !defined(_WIN32)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#endif

#define NIC_BASE 0xFEF00000u
#define NIC_SIZE 0x1000u
#define NIC_VECTOR 4
#define MMIO_SIZE 0x400

/* registers (nvnet_regs.h) */
#define IRQ_STATUS 0x000
#define  IRQ_RX 0x02
#define  IRQ_TX 0x10
#define  IRQ_MIIEVENT 0x40
#define IRQ_MASK 0x004
#define TRANSMITTER_CONTROL 0x084
#define PACKET_FILTER 0x08C
#define  PF_MYADDR 0x20
#define RECEIVER_CONTROL 0x094
#define MAC_ADDR_A 0x0A8
#define MAC_ADDR_B 0x0AC
#define MCAST_ADDR_A 0x0B0
#define MCAST_ADDR_B 0x0B4
#define MCAST_MASK_A 0x0B8
#define MCAST_MASK_B 0x0BC
#define TX_RING 0x100
#define RX_RING 0x104
#define RING_SIZE 0x108
#define TX_CUR 0x11C
#define RX_CUR 0x120
#define SETUP_REG5 0x130
#define TX_NEXT 0x134
#define RX_NEXT 0x138
#define TXRX_CONTROL 0x144
#define  TXRX_KICK 0x01
#define  TXRX_BIT1 0x02
#define  TXRX_BIT2 0x04
#define  TXRX_IDLE 0x08
#define  TXRX_RESET 0x10
#define MII_STATUS 0x180
#define  MII_LINKCHANGE 0x08
#define ADAPTER_CONTROL 0x188
#define  AC_LINKUP 0x04
#define MDIO_ADDR 0x190
#define  MDIO_INUSE 0x8000
#define  MDIO_WRITE 0x0400
#define MDIO_DATA 0x194

#define NV_TX_LASTPACKET (1 << 0)
#define NV_TX_VALID (1 << 15)
#define NV_TX_ERRBITS ((1 << 3) | (1 << 10) | (1 << 11) | (1 << 12) | (1 << 13) | (1 << 14))
#define NV_RX_DESCRIPTORVALID (1 << 0)
#define NV_RX_BIT4 (1 << 4)
#define NV_RX_AVAIL (1 << 15)

/* PHY (MII): BMCR full duplex + autoneg, BMSR link up + autoneg done,
 * ANAR / ANLPAR 10/100 half/full + T4 */
#define PHY_ADDR 1
static const uint16_t phy_init[6] = { 0x1100, 0x782C, 0, 0, 0x03E1, 0x43E1 };

typedef void (*recomp_func_t)(void);
extern recomp_func_t recomp_lookup(uint32_t xbox_va);
extern int xbox_worker_stack_alloc(void);
extern void xbox_worker_stack_free(int slot);
extern uint32_t xbox_GetConnectedInterrupt(uint32_t vector);
#include "../kernel/recomp_cpu_tls.h" /* was: extern __thread uint32_t g_eax, g_ecx, g_edx, g_esp, g_ebx, g_esi, g_edi, g_fs_base; */
extern uint32_t xbox_AllocThreadTib(void);

typedef struct { uint32_t addr; uint16_t len; uint16_t flags; } __attribute__((packed)) RingDesc;

static struct {
    int on;
    uint8_t regs[MMIO_SIZE];
    uint16_t phy[6];
    uint8_t txbuf[2048];
    unsigned txoff;
    uint8_t mac[6];
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int sock, port;
    uint32_t bcast[4]; int nbcast;
    uint32_t self_ip[4]; int nself;
    struct { uint8_t mac[6]; uint32_t ip; } peers[16];
    int npeers;
    /* frames received, waiting for an RX buffer */
    struct { uint16_t len; uint8_t data[1600]; } q[64];
    int qh, qt;
    unsigned tx_frames, rx_frames, rx_dropped, irqs;
    unsigned rx_heard, rx_filtered, rx_logged;
} N = { .lock = PTHREAD_MUTEX_INITIALIZER, .cond = PTHREAD_COND_INITIALIZER, .sock = -1 };

/* A bus master's physical address: the descriptor rings and packet buffers
 * come from MmAllocateContiguousMemory, whose bytes live in the contiguous
 * window here, not at the same number in low RAM. Read as low RAM (as
 * before), every frame went out all zeroes -- MAC 00:00:00:00:00:00, type 0
 * -- and no other console took it for a System Link packet. Resolved as the
 * APU resolves its DMA (heap pages named through MmGetPhysicalAddress,
 * contiguous pages, the image, the extended VMA). */
extern uint8_t *mcpx_apu_phys(uint64_t addr);
static uint8_t *G(uint32_t pa) { return mcpx_apu_phys(pa); }
static uint32_t rd(uint32_t a) { uint32_t v; memcpy(&v, N.regs + a, 4); return v; }
static void wr(uint32_t a, uint32_t v) { memcpy(N.regs + a, &v, 4); }
static void orr(uint32_t a, uint32_t v) { wr(a, rd(a) | v); }
static void andr(uint32_t a, uint32_t v) { wr(a, rd(a) & v); }

static unsigned tx_ring_n(void) { return (rd(RING_SIZE) & 0xFFFF) + 1; }
static unsigned rx_ring_n(void) { return (rd(RING_SIZE) >> 16) + 1; }
static int link_up(void) { return (N.phy[1] & 0x0004) != 0; }
static int dma_on(void) { return (rd(TXRX_CONTROL) & TXRX_BIT2) == 0; }

static void irq_kick(void) { pthread_cond_signal(&N.cond); }
static void set_irq(uint32_t bits) { orr(IRQ_STATUS, bits); if (rd(IRQ_MASK) & rd(IRQ_STATUS)) irq_kick(); }

/* ---- the wire: UDP on the phone's network ---------------------------------- */

#define WIRE_MAGIC 0x314B4E43u   /* "CNK1" */

static void net_learn(const uint8_t *mac, uint32_t ip)
{
    for (int i = 0; i < N.npeers; i++)
        if (!memcmp(N.peers[i].mac, mac, 6)) { N.peers[i].ip = ip; return; }
    if (N.npeers < 16) {
        memcpy(N.peers[N.npeers].mac, mac, 6);
        N.peers[N.npeers++].ip = ip;
        fprintf(stderr, "[NET] peer %02x:%02x:%02x:%02x:%02x:%02x at %s\n",
                mac[0], mac[1], mac[2], mac[3], mac[4], mac[5],
                inet_ntoa((struct in_addr){ ip }));
    }
}

/* Wire format "CNK2": magic, this console's frame sequence number, the
 * Ethernet frame. A frame for one console goes to it directly (unicast) and,
 * once more, to the LAN broadcast address: on the user's network the host's
 * unicast replies never reached the other phone (its key-exchange answers,
 * so the client gave up: "This game is no longer available") while
 * broadcasts did both ways. The receiver keeps whichever copy comes first
 * and drops the other by its sequence number. */
#define WIRE_MAGIC2 0x324B4E43u   /* "CNK2" */
static uint32_t s_tx_seq;

static void net_send(const uint8_t *frame, unsigned len)
{
    if (N.sock < 0 || len < 14 || len > 1600) return;
    uint8_t pkt[1608];
    uint32_t m = WIRE_MAGIC2, seq = ++s_tx_seq;
    memcpy(pkt, &m, 4);
    memcpy(pkt + 4, &seq, 4);
    memcpy(pkt + 8, frame, len);
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(N.port) };
    if (!(frame[0] & 1)) {
        for (int i = 0; i < N.npeers; i++)
            if (!memcmp(N.peers[i].mac, frame, 6)) {
                to.sin_addr.s_addr = N.peers[i].ip;
                sendto(N.sock, pkt, len + 8, 0, (struct sockaddr *)&to, sizeof to);
                break;
            }
    }
    /* broadcast / multicast / unknown unicast, and every unicast once more */
    for (int i = 0; i < N.nbcast; i++) {
        to.sin_addr.s_addr = N.bcast[i];
        sendto(N.sock, pkt, len + 8, 0, (struct sockaddr *)&to, sizeof to);
    }
}

/* duplicates (a frame's unicast and broadcast copies): per peer MAC, the
 * sequence numbers seen lately */
static struct { uint8_t mac[6]; uint32_t seen[64]; int n; } s_dup[16];
static unsigned s_dups;
static int seen_before(const uint8_t *mac, uint32_t seq)
{
    int k = -1;
    for (int i = 0; i < 16; i++) if (s_dup[i].n && !memcmp(s_dup[i].mac, mac, 6)) { k = i; break; }
    if (k < 0) for (int i = 0; i < 16; i++) if (!s_dup[i].n) { k = i; memcpy(s_dup[i].mac, mac, 6); break; }
    if (k < 0) return 0;
    for (int i = 0; i < 64 && i < s_dup[k].n; i++) if (s_dup[k].seen[i] == seq) { s_dups++; return 1; }
    s_dup[k].seen[s_dup[k].n % 64] = seq;
    s_dup[k].n++;
    if (s_dup[k].n >= 1 << 30) s_dup[k].n = 64;
    return 0;
}

static void net_find_bcast(void)
{
    N.nbcast = 0;
    struct ifaddrs *ifa = NULL;
    if (getifaddrs(&ifa) == 0) {
        for (struct ifaddrs *i = ifa; i && N.nbcast < 3; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET) continue;
            if ((i->ifa_flags & IFF_LOOPBACK) || !(i->ifa_flags & IFF_UP)) continue;
            if (!(i->ifa_flags & IFF_BROADCAST) || !i->ifa_broadaddr) continue;
            /* the phone's Wi-Fi (wlan*), and any other LAN it is on, not
             * the mobile data link */
            if (strncmp(i->ifa_name, "wlan", 4) && strncmp(i->ifa_name, "eth", 3) &&
                strncmp(i->ifa_name, "ap", 2) && strncmp(i->ifa_name, "swlan", 5)) continue;
            N.bcast[N.nbcast++] = ((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr.s_addr;
            if (N.nself < 4) N.self_ip[N.nself++] = ((struct sockaddr_in *)i->ifa_addr)->sin_addr.s_addr;
            fprintf(stderr, "[NET] LAN %s: broadcast %s\n", i->ifa_name,
                    inet_ntoa(((struct sockaddr_in *)i->ifa_broadaddr)->sin_addr));
        }
        freeifaddrs(ifa);
    }
    N.bcast[N.nbcast++] = 0xFFFFFFFFu;
}

/* ---- descriptor rings ------------------------------------------------------ */

static RingDesc load_desc(uint32_t a) { RingDesc d; memcpy(&d, G(a), sizeof d); return d; }
static void store_desc(uint32_t a, RingDesc d) { memcpy(G(a), &d, sizeof d); }

static uint32_t cur_desc(uint32_t base_reg, uint32_t next_reg, uint32_t cur_reg, unsigned n)
{
    uint32_t base = rd(base_reg), max = base + n * sizeof(RingDesc), c = rd(next_reg);
    if (c < base || c + sizeof(RingDesc) > max) c = base;
    wr(cur_reg, c);
    return c;
}
static void advance(uint32_t base_reg, uint32_t next_reg, uint32_t cur_reg, unsigned n)
{
    uint32_t base = rd(base_reg), max = base + n * sizeof(RingDesc);
    uint32_t nx = rd(cur_reg) + sizeof(RingDesc);
    if (nx >= max) nx = base;
    wr(next_reg, nx);
}

static void frame_log(const char *dir, const uint8_t *f, unsigned len, unsigned *count);
static unsigned s_flog_tx, s_flog_rx;

static void tx_kick(void)
{
    if (!(rd(TRANSMITTER_CONTROL) & 1) || !dma_on() || !link_up()) return;
    andr(TXRX_CONTROL, ~TXRX_IDLE);
    int sent = 0;
    unsigned n = tx_ring_n();
    for (unsigned i = 0; i < n; i++) {
        uint32_t da = cur_desc(TX_RING, TX_NEXT, TX_CUR, n);
        RingDesc d = load_desc(da);
        unsigned len = (unsigned)d.len + 1;
        if (!(d.flags & NV_TX_VALID)) break;
        if (N.txoff + len <= sizeof N.txbuf) {
            memcpy(N.txbuf + N.txoff, G(d.addr), len);
            N.txoff += len;
        }
        int last = d.flags & NV_TX_LASTPACKET;
        if (last) {
            if (N.tx_frames < 12) {
                const uint8_t *f = N.txbuf;
                fprintf(stderr, "[NET] tx %u: %02x:%02x:%02x:%02x:%02x:%02x <- %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x, %u bytes (MAC regs %08x %08x)\n",
                        N.tx_frames, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
                        f[12], f[13], N.txoff, rd(MAC_ADDR_A), rd(MAC_ADDR_B));
            }
            frame_log("TX", N.txbuf, N.txoff, &s_flog_tx);
            net_send(N.txbuf, N.txoff);
            N.tx_frames++;
            N.txoff = 0;
            sent = 1;
        }
        d.flags &= ~(NV_TX_VALID | NV_TX_ERRBITS);
        store_desc(da, d);
        advance(TX_RING, TX_NEXT, TX_CUR, n);
        /* every valid descriptor, as xemu's nvnet does: stopping after the
         * first packet left the rest of a burst in the ring until the next
         * kick, and once the ring filled the title's stack stopped sending
         * (the System Link client went quiet 20 s into a match) */
    }
    orr(TXRX_CONTROL, TXRX_IDLE);
    if (sent) set_irq(IRQ_TX);
}


/* One line per frame (System Link debugging): MACs, IPv4 addresses and UDP
 * ports. DHCP (UDP 67/68) is left out: the consoles ask for an address
 * every few seconds while no server answers. Up to 400 per direction. */
/* System Link traffic (not DHCP) seen lately: the game layer keeps the
 * single-player aiming layout off in a multiplayer match. */
static volatile int64_t s_game_traffic_ms;
int xbox_NicSessionActive(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t now = t.tv_sec * 1000LL + t.tv_nsec / 1000000;
    return s_game_traffic_ms && now - s_game_traffic_ms < 10000;
}

/* the network log also goes to <dump>/net_log.txt: logcat on the Pixel 8
 * kept only the last minute, and the start of a match was gone */
static FILE *s_netlog;
static void netlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void netlog(const char *fmt, ...)
{
    char b[512];
    va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    fputs(b, stderr);
    if (!s_netlog) {
        static int tried;
        const char *dd = getenv("NV2A_DUMP_DIR");
        if (!tried && dd) { tried = 1; char p[512]; snprintf(p, sizeof p, "%s/net_log.txt", dd); s_netlog = fopen(p, "w"); }
    }
    if (s_netlog) {
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
        struct tm tm; time_t sec = t.tv_sec; localtime_r(&sec, &tm);
        fprintf(s_netlog, "%02d:%02d:%02d.%03ld %s", tm.tm_hour, tm.tm_min, tm.tm_sec, t.tv_nsec / 1000000, b);
        fflush(s_netlog);
    }
}

static void frame_log(const char *dir, const uint8_t *f, unsigned len, unsigned *count)
{
    if (len < 14) return;
    char ipb[96] = "";
    if (f[12] == 0x08 && f[13] == 0x00 && len >= 34) {
        const uint8_t *ip = f + 14;
        unsigned ihl = (ip[0] & 15) * 4, proto = ip[9];
        unsigned sp = 0, dp = 0;
        if (proto == 17 && len >= 14 + ihl + 8) {
            sp = (ip[ihl] << 8) | ip[ihl + 1]; dp = (ip[ihl + 2] << 8) | ip[ihl + 3];
            if (sp == 67 || sp == 68 || dp == 67 || dp == 68) return;
        }
        {   struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
            s_game_traffic_ms = t.tv_sec * 1000LL + t.tv_nsec / 1000000; }
        snprintf(ipb, sizeof ipb, " ip %u.%u.%u.%u:%u -> %u.%u.%u.%u:%u proto %u",
                 ip[12], ip[13], ip[14], ip[15], sp, ip[16], ip[17], ip[18], ip[19], dp, proto);
    }
    if (*count >= 3000) return;
    (*count)++;
    netlog("[NET] %s %02x:%02x:%02x:%02x:%02x:%02x <- %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x len %u%s\n",
            dir, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
            f[12], f[13], len, ipb);
}

static int rx_filter(const uint8_t *b, unsigned len)
{
    if (len < 6) return 0;
    if (!memcmp(b, "\xff\xff\xff\xff\xff\xff", 6)) return 1;
    if (!(rd(PACKET_FILTER) & PF_MYADDR)) return 1;
    uint32_t a[2] = { rd(MCAST_ADDR_A), rd(MCAST_ADDR_B) };
    if (memcmp(a, "\xff\xff\xff\xff\xff\xff", 6)) {
        uint32_t d[2] = { 0, 0 };
        memcpy(d, b, 6);
        d[0] &= rd(MCAST_MASK_A); d[1] &= rd(MCAST_MASK_B);
        if (!memcmp(d, a, 6)) return 1;
    }
    a[0] = rd(MAC_ADDR_A); a[1] = rd(MAC_ADDR_B);
    return !memcmp(b, a, 6);
}

/* One queued frame into the guest's RX ring; 0 when it has no buffer. */
static int rx_deliver(const uint8_t *b, unsigned len)
{
    if (!(rd(RECEIVER_CONTROL) & 1) || !dma_on() || !link_up()) return 0;
    unsigned n = rx_ring_n();
    uint32_t da = cur_desc(RX_RING, RX_NEXT, RX_CUR, n);
    RingDesc d = load_desc(da);
    if (!(d.flags & NV_RX_AVAIL)) return 0;
    andr(TXRX_CONTROL, ~TXRX_IDLE);
    if (len > (unsigned)d.len + 1) len = (unsigned)d.len + 1;
    memcpy(G(d.addr), b, len);
    d.len = (uint16_t)len;
    d.flags = NV_RX_BIT4 | NV_RX_DESCRIPTORVALID;
    store_desc(da, d);
    advance(RX_RING, RX_NEXT, RX_CUR, n);
    orr(TXRX_CONTROL, TXRX_IDLE);
    set_irq(IRQ_RX);
    N.rx_frames++;
    return 1;
}

static void rx_drain(void)
{
    while (N.qh != N.qt) {
        if (!rx_deliver(N.q[N.qt].data, N.q[N.qt].len)) return;
        N.qt = (N.qt + 1) % 64;
    }
}

/* ---- registers ------------------------------------------------------------- */

static uint64_t nic_read(void *dev, uint32_t off, int size)
{
    (void)dev;
    uint64_t v = 0;
    if (off + size > MMIO_SIZE) return 0;
    pthread_mutex_lock(&N.lock);
    memcpy(&v, N.regs + off, size);
    pthread_mutex_unlock(&N.lock);
    return v;
}

static void nic_write(void *dev, uint32_t off, uint64_t val64, int size)
{
    (void)dev;
    if (off + size > MMIO_SIZE) return;
    uint32_t val = (uint32_t)val64;
    pthread_mutex_lock(&N.lock);
    switch (off) {
    case MDIO_ADDR: {
        wr(MDIO_ADDR, val);
        unsigned pa = (val >> 5) & 0x1F, reg = val & 0x1F;
        if (val & MDIO_WRITE) {
            if (pa == PHY_ADDR && reg < 6) N.phy[reg] = (uint16_t)rd(MDIO_DATA);
        } else {
            wr(MDIO_DATA, pa == PHY_ADDR ? (reg < 6 ? N.phy[reg] : 0) : 0xFFFFFFFFu);
        }
        andr(MDIO_ADDR, ~MDIO_INUSE);
        break;
    }
    case TXRX_CONTROL:
        wr(TXRX_CONTROL, (rd(TXRX_CONTROL) & (val | TXRX_IDLE)) | (val & ~TXRX_IDLE));
        if (val & TXRX_KICK) tx_kick();
        if (val & TXRX_RESET) {
            wr(TX_CUR, rd(TX_RING)); wr(TX_NEXT, rd(TX_RING));
            wr(RX_CUR, rd(RX_RING)); wr(RX_NEXT, rd(RX_RING));
            N.txoff = 0;
        }
        if (val & TXRX_BIT1) wr(IRQ_STATUS, 0);
        else if (val == 0) wr(SETUP_REG5, 1u << 31);
        break;
    case IRQ_STATUS:
    case MII_STATUS: {
        uint32_t cur = 0; memcpy(&cur, N.regs + off, size);
        cur &= ~val; memcpy(N.regs + off, &cur, size);
        break;
    }
    case IRQ_MASK:
        memcpy(N.regs + off, &val, size);
        if (rd(IRQ_MASK) & rd(IRQ_STATUS)) irq_kick();
        break;
    case RECEIVER_CONTROL:
        memcpy(N.regs + off, &val, size);
        irq_kick();   /* a started receiver can take what is queued */
        break;
    default:
        memcpy(N.regs + off, &val, size);
        break;
    }
    pthread_mutex_unlock(&N.lock);
}

/* ---- the interrupt ----------------------------------------------------------- */

/* The NIC's interrupt vector as the title connected it. XNet reads the
 * interrupt line from the NIC's PCI config space, which this kernel serves
 * zeroed: the title connects its NIC ISR (it reads IRQ_MASK, 0xFEF00004) on
 * vector 0, not 4. Nothing was ever delivered there, so the title never
 * took a received frame: the host heard the other phone, the game did not. */
static uint32_t nic_vector(void)
{
    if (xbox_GetConnectedInterrupt(NIC_VECTOR)) return NIC_VECTOR;
    if (xbox_GetConnectedInterrupt(0)) return 0;
    return NIC_VECTOR;
}

static int call_isr(void)
{
    uint32_t kint = xbox_GetConnectedInterrupt(nic_vector());
    if (!kint) return -1;
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t routine = *(uint32_t *)(mem + kint), context = *(uint32_t *)(mem + kint + 4);
    recomp_func_t fn = routine ? recomp_lookup(routine) : NULL;
    if (!fn) return -1;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = context;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = kint;
    g_esp -= 4; *(uint32_t *)(mem + g_esp) = 0xDEADBEEFu;
    int irql = xbox_IrqlEnterInterrupt(16);
    fn();
    xbox_IrqlLeaveInterrupt(irql);
    N.irqs++;
    return (int)(g_eax & 1u);
}

static void isr_on_holder(void) { call_isr(); }

static int irq_pending(void)
{
    pthread_mutex_lock(&N.lock);
    int p = (rd(IRQ_MASK) & rd(IRQ_STATUS)) != 0;
    pthread_mutex_unlock(&N.lock);
    return p;
}

static void *nic_thread(void *arg)
{
    (void)arg;
    uint32_t tib = xbox_AllocThreadTib();
    if (!tib) { fprintf(stderr, "[NET] no TIB for the NIC thread\n"); return NULL; }
    g_fs_base = tib;
    xbox_IrqSetHandler(XBOX_IRQ_NIC, isr_on_holder);
    unsigned last_log = 0;
    struct timespec st0; clock_gettime(CLOCK_MONOTONIC, &st0);
    for (;;) {
        {   /* every 10 s: what came in, what reached the game, the NIC state */
            struct timespec nw; clock_gettime(CLOCK_MONOTONIC, &nw);
            if (nw.tv_sec - st0.tv_sec >= 10) {
                st0 = nw;
                pthread_mutex_lock(&N.lock);
                unsigned n = rx_ring_n(), avail = 0;
                uint32_t base = rd(RX_RING);
                if (base) for (unsigned i = 0; i < n && i < 256; i++)
                    if (load_desc(base + i * sizeof(RingDesc)).flags & NV_RX_AVAIL) avail++;
                netlog("[NET] stats: tx %u; heard %u (+%u duplicates), filtered %u, delivered %u, queued %d, dropped %u; "
                                "irqs %u; peers %d; irqmask %08x status %08x rxctl %08x filter %08x mac %08x %08x; "
                                "rx ring %u/%u free, isr %s\n",
                        N.tx_frames, N.rx_heard, s_dups, N.rx_filtered, N.rx_frames, (N.qh - N.qt + 64) % 64,
                        N.rx_dropped, N.irqs, N.npeers, rd(IRQ_MASK), rd(IRQ_STATUS), rd(RECEIVER_CONTROL),
                        rd(PACKET_FILTER), rd(MAC_ADDR_A), rd(MAC_ADDR_B), avail, n,
                        xbox_GetConnectedInterrupt(nic_vector()) ? (nic_vector() ? "connected (4)" : "connected (0)") : "NOT connected");
                pthread_mutex_unlock(&N.lock);
            }
        }
        pthread_mutex_lock(&N.lock);
        struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += 5 * 1000000L;
        if (dl.tv_nsec >= 1000000000L) { dl.tv_sec++; dl.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&N.cond, &N.lock, &dl);
        rx_drain();
        pthread_mutex_unlock(&N.lock);

        if (!irq_pending() || !xbox_GetConnectedInterrupt(nic_vector())) continue;
        int gated = 0;
        if (xbox_IrqSafePointsOn()) {
            int how = xbox_IrqPost(XBOX_IRQ_NIC);
            if (how == XBOX_IRQ_POSTED) continue;
            gated = how == XBOX_IRQ_GATED;
        } else {
            gated = xbox_DispatchGateOn() ? xbox_DispatchGateTryEnter() : !xbox_IrqlBlocksInterrupts();
            if (!gated) continue;   /* the next pass, 5 ms on */
        }
        int slot = xbox_worker_stack_alloc();
        if (slot >= 0) {
            g_esp = XBOX_WORKER_STACK_TOP(slot);
            g_eax = g_ecx = g_edx = g_ebx = g_esi = g_edi = 0;
            call_isr();
            xbox_worker_stack_free(slot);
        }
        if (gated && xbox_DispatchGateOn()) xbox_DispatchGateLeave();
        if (N.irqs - last_log >= 200 || (N.irqs && !last_log)) {
            last_log = N.irqs;
            fprintf(stderr, "[NET] irqs %u, tx %u, rx %u (dropped %u), peers %d\n",
                    N.irqs, N.tx_frames, N.rx_frames, N.rx_dropped, N.npeers);
        }
    }
    return NULL;
}

static void *net_rx_thread(void *arg)
{
    (void)arg;
    uint8_t pkt[1700];
    for (;;) {
        struct sockaddr_in from; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(N.sock, pkt, sizeof pkt, 0, (struct sockaddr *)&from, &fl);
        if (n < 4 + 14) continue;
        uint32_t m; memcpy(&m, pkt, 4);
        const uint8_t *f; unsigned len; uint32_t seq = 0;
        if (m == WIRE_MAGIC2 && n >= 8 + 14) { memcpy(&seq, pkt + 4, 4); f = pkt + 8; len = (unsigned)n - 8; }
        else if (m == WIRE_MAGIC) { f = pkt + 4; len = (unsigned)n - 4; }   /* older builds */
        else continue;
        if (!memcmp(f + 6, N.mac, 6)) continue;   /* our own broadcast */
        {   int self = 0;
            for (int i = 0; i < N.nself; i++) if (from.sin_addr.s_addr == N.self_ip[i]) self = 1;
            if (self) continue; }
        pthread_mutex_lock(&N.lock);
        net_learn(f + 6, from.sin_addr.s_addr);
        if (seq && seen_before(f + 6, seq)) { pthread_mutex_unlock(&N.lock); continue; }
        N.rx_heard++;
        int pass = rx_filter(f, len);
        if (N.rx_logged < 24) {
            N.rx_logged++;
            fprintf(stderr, "[NET] rx %u: %02x:%02x:%02x:%02x:%02x:%02x <- %02x:%02x:%02x:%02x:%02x:%02x type %02x%02x, %u bytes from %s: %s\n",
                    N.rx_heard, f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11],
                    f[12], f[13], len, inet_ntoa(from.sin_addr), pass ? "to the game" : "filtered out");
        }
        if (!pass) N.rx_filtered++;
        frame_log(pass ? "RX" : "RX(filtered)", f, len, &s_flog_rx);
        if (pass) {
            int nh = (N.qh + 1) % 64;
            if (nh == N.qt) N.rx_dropped++;
            else { N.q[N.qh].len = (uint16_t)len; memcpy(N.q[N.qh].data, f, len); N.qh = nh; }
            pthread_cond_signal(&N.cond);
        }
        pthread_mutex_unlock(&N.lock);
    }
    return NULL;
}

/* ---- bring-up -------------------------------------------------------------- */

const uint8_t *xbox_NicMac(void) { return N.mac; }

static void mac_init(void)
{
    const char *e = getenv("CONKER_MAC");
    unsigned b[6];
    if (e && sscanf(e, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        for (int i = 0; i < 6; i++) N.mac[i] = (uint8_t)b[i];
    } else {
        /* Xbox OUI 00:50:F2 and something from this run */
        struct timespec t; clock_gettime(CLOCK_REALTIME, &t);
        uint32_t r = (uint32_t)(t.tv_nsec ^ (t.tv_sec * 2654435761u) ^ getpid());
        uint8_t m[6] = { 0x00, 0x50, 0xF2, (uint8_t)(r >> 16), (uint8_t)(r >> 8), (uint8_t)r };
        memcpy(N.mac, m, 6);
    }
}

void xbox_NicProbeInit(void)
{
#if defined(__aarch64__) || (defined(__x86_64__) && defined(__linux__))
    mac_init();
    if (getenv("CONKER_NO_NIC") || getenv("CONKER_NO_NIC_PROBE")) return;
    ptrdiff_t off = xbox_GetMemoryOffset();
    if (!off) return;
    DWORD old;
    if (!VirtualProtect((LPVOID)((uintptr_t)off + NIC_BASE), NIC_SIZE, PAGE_NOACCESS, &old)) {
        fprintf(stderr, "[NET] cannot trap the NIC at 0x%08X\n", NIC_BASE);
        return;
    }
    memset(N.regs, 0, sizeof N.regs);
    orr(TXRX_CONTROL, TXRX_IDLE);
    memcpy(N.phy, phy_init, sizeof N.phy);
    orr(ADAPTER_CONTROL, AC_LINKUP);
    N.on = 1;

    const char *p = getenv("CONKER_NET_PORT");
    N.port = p ? atoi(p) : 39123;
    N.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (N.sock >= 0) {
        int one = 1;
        setsockopt(N.sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
        setsockopt(N.sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(N.port),
                                 .sin_addr.s_addr = htonl(INADDR_ANY) };
        if (bind(N.sock, (struct sockaddr *)&a, sizeof a) < 0) {
            fprintf(stderr, "[NET] cannot bind UDP %d\n", N.port);
            close(N.sock); N.sock = -1;
        }
    }
    net_find_bcast();
    pthread_t t;
    pthread_create(&t, NULL, nic_thread, NULL); pthread_detach(t);
    if (N.sock >= 0) { pthread_create(&t, NULL, net_rx_thread, NULL); pthread_detach(t); }
    fprintf(stderr, "[NET] NIC at 0x%08X, MAC %02x:%02x:%02x:%02x:%02x:%02x, UDP %d%s\n",
            NIC_BASE, N.mac[0], N.mac[1], N.mac[2], N.mac[3], N.mac[4], N.mac[5], N.port,
            N.sock < 0 ? " (no socket: offline)" : "");
#endif
}

int xbox_NicOwnsAddress(uint32_t va)
{
    return N.on && va >= NIC_BASE && va < NIC_BASE + NIC_SIZE;
}

int xbox_NicHandleMmio(void *ctx, uint32_t va)
{
#if defined(__aarch64__)
    return mmio_emulate_a64((ucontext_t *)ctx, va - NIC_BASE, NULL, nic_read, nic_write);
#elif defined(__x86_64__) && defined(__linux__)
    return mmio_emulate_x86_64((ucontext_t *)ctx, va - NIC_BASE, NULL, nic_read, nic_write);
#else
    (void)ctx; (void)va;
    return 0;
#endif
}
