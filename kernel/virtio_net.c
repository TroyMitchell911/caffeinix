#include <virtio_net.h>
#include <mem_layout.h>
#include <spinlock.h>
#include <debug.h>
#include <mystring.h>
#include <palloc.h>
#include <printf.h>

#define NUM                     VIRTIO_DES_NUM
#define R(r)                    ((volatile uint32 *)(VIRTIO1 + (r)))
#define C(r)                    ((volatile uint8 *)(VIRTIO1 + VIRTIO_MMIO_CONFIG + (r)))

static struct net {
        struct virtq_desc *desc[2];
        struct virtq_avail *avail[2];
        struct virtq_used *used[2];
        uint16 used_idx[2];
        uint8 ready;
        uint8 mac[6];
        struct spinlock vnet_lock;
} net;

static void virtio_net_queue_init(int q)
{
        *R(VIRTIO_MMIO_QUEUE_SEL) = q;

        if(*R(VIRTIO_MMIO_QUEUE_READY))
                PANIC("virtio net queue already ready");

        uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
        if(max == 0)
                PANIC("virtio net queue missing");
        if(max < NUM)
                PANIC("virtio net queue too short");

        net.desc[q] = palloc();
        net.avail[q] = palloc();
        net.used[q] = palloc();
        if(!net.desc[q] || !net.avail[q] || !net.used[q])
                PANIC("virtio net kalloc");

        memset(net.desc[q], 0, PGSIZE);
        memset(net.avail[q], 0, PGSIZE);
        memset(net.used[q], 0, PGSIZE);

        *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;
        *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)net.desc[q];
        *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)net.desc[q] >> 32;
        *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)net.avail[q];
        *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)net.avail[q] >> 32;
        *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)net.used[q];
        *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)net.used[q] >> 32;
        *R(VIRTIO_MMIO_QUEUE_READY) = 1;

        net.used_idx[q] = 0;
}

void virtio_net_init(void)
{
        uint32 status = 0;

        spinlock_init(&net.vnet_lock, "virtio_net");

        if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
           *R(VIRTIO_MMIO_VERSION) != 2 ||
           *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551) {
                printf("virtio net: device not present\n");
                return;
        }

        if(*R(VIRTIO_MMIO_DEVICE_ID) != VIRTIO_DEVICE_NET) {
                printf("virtio net: unexpected device id %d\n", *R(VIRTIO_MMIO_DEVICE_ID));
                return;
        }

        *R(VIRTIO_MMIO_STATUS) = status;

        status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
        *R(VIRTIO_MMIO_STATUS) = status;

        status |= VIRTIO_CONFIG_S_DRIVER;
        *R(VIRTIO_MMIO_STATUS) = status;

        uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
        uint64 negotiated = 0;
        if(features & (1 << VIRTIO_NET_F_MAC))
                negotiated |= (1 << VIRTIO_NET_F_MAC);
        *R(VIRTIO_MMIO_DRIVER_FEATURES) = negotiated;

        status |= VIRTIO_CONFIG_S_FEATURES_OK;
        *R(VIRTIO_MMIO_STATUS) = status;

        status = *R(VIRTIO_MMIO_STATUS);
        if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
                PANIC("virtio net FEATURES_OK unset");

        if(negotiated & (1 << VIRTIO_NET_F_MAC)) {
                for(int i = 0; i < 6; i++)
                        net.mac[i] = *C(i);
        }

        virtio_net_queue_init(VIRTIO_NET_RX_QUEUE);
        virtio_net_queue_init(VIRTIO_NET_TX_QUEUE);

        status |= VIRTIO_CONFIG_S_DRIVER_OK;
        *R(VIRTIO_MMIO_STATUS) = status;

        net.ready = 1;
        printf("virtio net: initialized");
        if(negotiated & (1 << VIRTIO_NET_F_MAC)) {
                printf(" mac=%x:%x:%x:%x:%x:%x",
                       net.mac[0], net.mac[1], net.mac[2],
                       net.mac[3], net.mac[4], net.mac[5]);
        }
        printf("\n");
}

void virtio_net_intr(void)
{
        if(!net.ready)
                return;

        uint32 status = *R(VIRTIO_MMIO_INTERRUPT_STATUS);
        if(status)
                *R(VIRTIO_MMIO_INTERRUPT_ACK) = status;
}
