#include "virtio_net.h"
#include "pci.h"
#include "pbuf.h"
#include "netdev.h"
#include "ethernet.h"
#include "pmm.h"
#include "paging.h"
#include "idt.h"
#include "io.h"
#include "console.h"
#include "serial.h"
#include "kmutex.h"
#include <string.h>

#define VIRTIO_VENDOR_ID 0x1AF4
#define VIRTIO_DEV_NET   0x1000

#define VIRTIO_NET_F_MAC    (1 << 5)
#define VIRTIO_NET_F_STATUS (1 << 16)

#define VRING_DESC_F_NEXT     1
#define VRING_DESC_F_WRITE    2

struct vring_desc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
} __attribute__((packed));

struct vring_avail {
    uint16_t flags;
    uint16_t idx;
    uint16_t ring[256];
} __attribute__((packed));

struct vring_used_elem {
    uint32_t id;
    uint32_t len;
} __attribute__((packed));

struct vring_used {
    uint16_t flags;
    uint16_t idx;
    struct vring_used_elem ring[256];
} __attribute__((packed));

struct virtio_net_hdr {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
} __attribute__((packed));

#define RX_BUFFER_COUNT 16
#define RX_BUF_SIZE     1524

static netdev_t s_eth0;
static uint16_t s_io_base = 0;
static uint8_t  s_irq_line = 0;

/* RX virtqueue */
static uint16_t s_rx_qsize = 0;
static struct vring_desc*  s_rx_desc = NULL;
static struct vring_avail* s_rx_avail = NULL;
static struct vring_used*  s_rx_used = NULL;
static uint16_t s_rx_last_used = 0;
static uint8_t* s_rx_buf_virt = NULL;
static uint64_t s_rx_buf_phys = 0;

/* TX virtqueue */
static uint16_t s_tx_qsize = 0;
static struct vring_desc*  s_tx_desc = NULL;
static struct vring_avail* s_tx_avail = NULL;
static struct vring_used*  s_tx_used = NULL;
static uint16_t s_tx_last_used = 0;
static uint8_t* s_tx_buf_virt = NULL;
static uint64_t s_tx_buf_phys = 0;
static kmutex_t s_tx_lock;

extern int net_killswitch_active(void);

static void virtio_net_irq(struct registers* regs) {
    (void)regs;
    if (!s_io_base) return;

    uint8_t isr = inb(s_io_base + 0x13);
    if (!(isr & 1)) return; /* Not a queue interrupt */

    if (net_killswitch_active()) {
        while (s_rx_last_used != s_rx_used->idx) {
            uint16_t id = (uint16_t) s_rx_used->ring[s_rx_last_used % s_rx_qsize].id;
            s_rx_last_used++;
            s_eth0.rx_dropped++;
            s_rx_avail->ring[s_rx_avail->idx % s_rx_qsize] = id;
            s_rx_avail->idx++;
        }
        outw(s_io_base + 0x10, 0);
        return;
    }

    while (s_rx_last_used != s_rx_used->idx) {
        uint16_t id = (uint16_t) s_rx_used->ring[s_rx_last_used % s_rx_qsize].id;
        uint32_t len = s_rx_used->ring[s_rx_last_used % s_rx_qsize].len;
        s_rx_last_used++;

        if (len > sizeof(struct virtio_net_hdr)) {
            uint32_t pkt_len = len - sizeof(struct virtio_net_hdr);
            pbuf_t* p = pbuf_alloc();
            if (p) {
                uint8_t* pkt_data = s_rx_buf_virt + (id * RX_BUF_SIZE) + sizeof(struct virtio_net_hdr);
                memcpy(p->payload, pkt_data, pkt_len);
                p->len = pkt_len;
                p->offset = 0;
                s_eth0.rx_packets++;
                s_eth0.rx_bytes += pkt_len;
                ethernet_rx(&s_eth0, p);
            } else {
                s_eth0.rx_dropped++;
            }
        }

        s_rx_avail->ring[s_rx_avail->idx % s_rx_qsize] = id;
        s_rx_avail->idx++;
    }
    outw(s_io_base + 0x10, 0);
}

static int virtio_net_send(netdev_t* dev, pbuf_t* pbuf) {
    if (!dev || !pbuf || pbuf->len > 1514 || !s_io_base) return -1;
    if (net_killswitch_active()) {
        dev->tx_dropped++;
        return -1;
    }

    kmutex_lock(&s_tx_lock);

    /* Zero header */
    memset(s_tx_buf_virt, 0, sizeof(struct virtio_net_hdr));
    /* Copy packet data */
    memcpy(s_tx_buf_virt + sizeof(struct virtio_net_hdr), pbuf->payload + pbuf->offset, pbuf->len);

    uint32_t total_len = sizeof(struct virtio_net_hdr) + pbuf->len;
    s_tx_desc[0].addr = s_tx_buf_phys;
    s_tx_desc[0].len = total_len;
    s_tx_desc[0].flags = 0;
    s_tx_desc[0].next = 0;

    s_tx_avail->ring[s_tx_avail->idx % s_tx_qsize] = 0;
    s_tx_avail->idx++;

    outw(s_io_base + 0x10, 1); /* Notify TX queue */

    /* Poll for completion with bounded timeout */
    uint32_t timeout = 500000;
    while (s_tx_used->idx == s_tx_last_used && timeout > 0) {
        timeout--;
        asm volatile ("pause");
    }

    if (timeout > 0) {
        s_tx_last_used = s_tx_used->idx;
        dev->tx_packets++;
        dev->tx_bytes += pbuf->len;
        kmutex_unlock(&s_tx_lock);
        return 0;
    } else {
        dev->tx_errors++;
        kmutex_unlock(&s_tx_lock);
        return -1;
    }
}

int virtio_net_init(void) {
    const pci_device_t* pci_dev = pci_find_device(VIRTIO_VENDOR_ID, VIRTIO_DEV_NET);
    if (!pci_dev) {
        kprintf("[virtio_net] Device not found on PCI bus\n");
        return -1;
    }

    pci_enable_bus_master(pci_dev);
    s_io_base = (uint16_t)(pci_dev->bars[0] & ~1u);
    s_irq_line = pci_dev->irq_line;

    kprintf("[virtio_net] Found device at IO 0x%x, IRQ %u\n", s_io_base, s_irq_line);

    /* Reset device */
    outb(s_io_base + 0x12, 0);
    outb(s_io_base + 0x12, 1); /* ACKNOWLEDGE */
    outb(s_io_base + 0x12, 1 | 2); /* DRIVER */

    /* Feature negotiation */
    uint32_t host_features = inl(s_io_base + 0x00);
    uint32_t guest_features = 0;
    if (host_features & VIRTIO_NET_F_MAC) {
        guest_features |= VIRTIO_NET_F_MAC;
    }
    outl(s_io_base + 0x04, guest_features);

    /* Read MAC */
    for (int i = 0; i < 6; i++) {
        s_eth0.mac[i] = inb(s_io_base + 0x14 + i);
    }
    kprintf("[virtio_net] MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
            s_eth0.mac[0], s_eth0.mac[1], s_eth0.mac[2],
            s_eth0.mac[3], s_eth0.mac[4], s_eth0.mac[5]);

    /* Setup RX Queue (0) */
    outw(s_io_base + 0x0E, 0);
    s_rx_qsize = inw(s_io_base + 0x0C);
    if (s_rx_qsize == 0 || s_rx_qsize > 256) {
        kprintf("[virtio_net] Invalid RX queue size %u\n", s_rx_qsize);
        return -1;
    }

    uint64_t rx_vring_phys = pmm_alloc_contiguous_frames(4);
    if (!rx_vring_phys) return -1;
    uint8_t* rx_vring_virt = (uint8_t*) P2V(rx_vring_phys);
    memset(rx_vring_virt, 0, 4 * FRAME_SIZE);

    s_rx_desc = (struct vring_desc*) rx_vring_virt;
    s_rx_avail = (struct vring_avail*) (rx_vring_virt + sizeof(struct vring_desc) * s_rx_qsize);
    uint32_t used_offset = ((sizeof(struct vring_desc) * s_rx_qsize + sizeof(uint16_t) * (3 + s_rx_qsize) + 4095) & ~4095u);
    s_rx_used = (struct vring_used*) (rx_vring_virt + used_offset);
    s_rx_last_used = 0;

    outl(s_io_base + 0x08, (uint32_t)(rx_vring_phys >> 12));

    /* Setup TX Queue (1) */
    outw(s_io_base + 0x0E, 1);
    s_tx_qsize = inw(s_io_base + 0x0C);
    if (s_tx_qsize == 0 || s_tx_qsize > 256) {
        kprintf("[virtio_net] Invalid TX queue size %u\n", s_tx_qsize);
        return -1;
    }

    uint64_t tx_vring_phys = pmm_alloc_contiguous_frames(4);
    if (!tx_vring_phys) return -1;
    uint8_t* tx_vring_virt = (uint8_t*) P2V(tx_vring_phys);
    memset(tx_vring_virt, 0, 4 * FRAME_SIZE);

    s_tx_desc = (struct vring_desc*) tx_vring_virt;
    s_tx_avail = (struct vring_avail*) (tx_vring_virt + sizeof(struct vring_desc) * s_tx_qsize);
    used_offset = ((sizeof(struct vring_desc) * s_tx_qsize + sizeof(uint16_t) * (3 + s_tx_qsize) + 4095) & ~4095u);
    s_tx_used = (struct vring_used*) (tx_vring_virt + used_offset);
    s_tx_last_used = 0;

    outl(s_io_base + 0x08, (uint32_t)(tx_vring_phys >> 12));

    /* Allocate DMA buffers */
    s_rx_buf_phys = pmm_alloc_contiguous_frames(8);
    if (!s_rx_buf_phys) return -1;
    s_rx_buf_virt = (uint8_t*) P2V(s_rx_buf_phys);
    memset(s_rx_buf_virt, 0, 8 * FRAME_SIZE);

    s_tx_buf_phys = pmm_alloc_frame();
    if (!s_tx_buf_phys) return -1;
    s_tx_buf_virt = (uint8_t*) P2V(s_tx_buf_phys);
    memset(s_tx_buf_virt, 0, FRAME_SIZE);

    kmutex_init(&s_tx_lock, "virtio_tx");

    /* Populate RX buffers */
    int fill_count = (s_rx_qsize < RX_BUFFER_COUNT) ? s_rx_qsize : RX_BUFFER_COUNT;
    for (int i = 0; i < fill_count; i++) {
        s_rx_desc[i].addr = s_rx_buf_phys + (i * RX_BUF_SIZE);
        s_rx_desc[i].len = RX_BUF_SIZE;
        s_rx_desc[i].flags = VRING_DESC_F_WRITE;
        s_rx_desc[i].next = 0;
        s_rx_avail->ring[i] = i;
    }
    s_rx_avail->idx = fill_count;

    /* Notify device of ready RX buffers */
    outw(s_io_base + 0x10, 0);

    /* Register IRQ handler */
    register_interrupt_handler(32 + s_irq_line, virtio_net_irq);

    /* Device status: DRIVER_OK */
    outb(s_io_base + 0x12, 1 | 2 | 4);

    /* Initialize netdev eth0 */
    memset(&s_eth0, 0, sizeof(netdev_t));
    strcpy(s_eth0.name, "eth0");
    /* Default QEMU slirp network configuration */
    s_eth0.ip = 0x0F02000Au;      /* 10.0.2.15 */
    s_eth0.netmask = 0x00FFFFFFu; /* 255.255.255.0 */
    s_eth0.gateway = 0x0202000Au; /* 10.0.2.2 */
    s_eth0.dns = 0x0302000Au;     /* 10.0.2.3 */
    s_eth0.flags = NETDEV_FLAG_UP | NETDEV_FLAG_RUNNING;
    s_eth0.mtu = 1500;
    s_eth0.send = virtio_net_send;

    netdev_register(&s_eth0);
    kprintf("[virtio_net] eth0 registered (10.0.2.15/24, gw 10.0.2.2)\n");

    return 0;
}
