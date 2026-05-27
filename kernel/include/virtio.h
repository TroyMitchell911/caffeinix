#ifndef __CAFFEINIX_KERNEL_VIRTIO_H
#define __CAFFEINIX_KERNEL_VIRTIO_H

#include <typedefs.h>

#define VIRTIO_MMIO_MAGIC_VALUE         0x000
#define VIRTIO_MMIO_VERSION             0x004
#define VIRTIO_MMIO_DEVICE_ID           0x008
#define VIRTIO_MMIO_VENDOR_ID           0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES     0x010
#define VIRTIO_MMIO_DRIVER_FEATURES     0x020
#define VIRTIO_MMIO_QUEUE_SEL           0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX       0x034
#define VIRTIO_MMIO_QUEUE_NUM           0x038
#define VIRTIO_MMIO_QUEUE_READY         0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY        0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS    0x060
#define VIRTIO_MMIO_INTERRUPT_ACK       0x064
#define VIRTIO_MMIO_STATUS              0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW      0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH     0x084
#define VIRTIO_MMIO_DRIVER_DESC_LOW     0x090
#define VIRTIO_MMIO_DRIVER_DESC_HIGH    0x094
#define VIRTIO_MMIO_DEVICE_DESC_LOW     0x0a0
#define VIRTIO_MMIO_DEVICE_DESC_HIGH    0x0a4
#define VIRTIO_MMIO_CONFIG              0x100

#define VIRTIO_DEVICE_NET               1
#define VIRTIO_DEVICE_BLOCK             2

#define VIRTIO_CONFIG_S_ACKNOWLEDGE     1
#define VIRTIO_CONFIG_S_DRIVER          2
#define VIRTIO_CONFIG_S_DRIVER_OK       4
#define VIRTIO_CONFIG_S_FEATURES_OK     8

#define VIRTIO_F_ANY_LAYOUT             27
#define VIRTIO_RING_F_INDIRECT_DESC     28
#define VIRTIO_RING_F_EVENT_IDX         29

#define VIRTIO_DES_NUM                  8

struct virtq_desc {
        uint64 addr;
        uint32 len;
        uint16 flags;
        uint16 next;
};

#define VRING_DESC_F_NEXT               1
#define VRING_DESC_F_WRITE              2

struct virtq_avail {
        uint16 flags;
        uint16 idx;
        uint16 ring[VIRTIO_DES_NUM];
        uint16 unused;
};

struct virtq_used_elem {
        uint32 id;
        uint32 len;
};

struct virtq_used {
        uint16 flags;
        uint16 idx;
        struct virtq_used_elem ring[VIRTIO_DES_NUM];
};

#endif
