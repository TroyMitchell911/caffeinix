#ifndef __CAFFEINIX_KERNEL_VIRTIO_NET_H
#define __CAFFEINIX_KERNEL_VIRTIO_NET_H

#include <virtio.h>

#define VIRTIO_NET_F_CSUM               0
#define VIRTIO_NET_F_GUEST_CSUM         1
#define VIRTIO_NET_F_MAC                5
#define VIRTIO_NET_F_GUEST_TSO4         7
#define VIRTIO_NET_F_GUEST_TSO6         8
#define VIRTIO_NET_F_GUEST_ECN          9
#define VIRTIO_NET_F_GUEST_UFO          10
#define VIRTIO_NET_F_HOST_TSO4          11
#define VIRTIO_NET_F_HOST_TSO6          12
#define VIRTIO_NET_F_HOST_ECN           13
#define VIRTIO_NET_F_HOST_UFO           14
#define VIRTIO_NET_F_MRG_RXBUF          15
#define VIRTIO_NET_F_STATUS             16
#define VIRTIO_NET_F_CTRL_VQ            17
#define VIRTIO_NET_F_CTRL_RX            18
#define VIRTIO_NET_F_CTRL_VLAN          19
#define VIRTIO_NET_F_GUEST_ANNOUNCE     21
#define VIRTIO_NET_F_MQ                 22
#define VIRTIO_NET_F_CTRL_MAC_ADDR      23

#define VIRTIO_NET_RX_QUEUE             0
#define VIRTIO_NET_TX_QUEUE             1

void virtio_net_init(void);
void virtio_net_intr(void);

#endif
