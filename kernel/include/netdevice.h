#ifndef __CAFFEINIX_KERNEL_NETDEVICE_H
#define __CAFFEINIX_KERNEL_NETDEVICE_H

#include <kernel_config.h>
#include <list.h>
#include <spinlock.h>
#include <typedefs.h>
#include <wait.h>

#define NET_DEVICE_MAX 8
#define NET_DEVICE_NAME_SIZE 16
#define NET_ETH_ADDRESS_LENGTH 6
#define NET_ETH_HEADER_LENGTH 14
#define NET_ETH_MTU 1500
#define NET_PACKET_SIZE 2048
#define NET_PACKET_HEADROOM 64
#define NET_PACKET_POOL_SIZE 128

#define NETDEV_TX_BUSY -2

struct device;
struct net_device;

struct net_packet {
	struct spinlock lock;
	struct net_device *device;
	uint8 *buffer;
	uint8 *data;
	uint32 capacity;
	uint32 length;
	uint32 refcount;
	struct net_packet *next_free;
	struct net_packet *next_receive;
	uint8 pooled;
};

struct net_device_operations {
	int (*open)(struct net_device *device);
	void (*stop)(struct net_device *device);
	int (*start_xmit)(struct net_device *device,
			  struct net_packet *packet);
};

struct net_device_stats {
	uint64 rx_packets;
	uint64 rx_bytes;
	uint64 rx_dropped;
	uint64 tx_packets;
	uint64 tx_bytes;
	uint64 tx_dropped;
};

struct net_device {
	struct spinlock lock;
	struct wait_queue lifecycle_wait;
	char name[NET_DEVICE_NAME_SIZE];
	uint8 address[NET_ETH_ADDRESS_LENGTH];
	uint32 mtu;
	uint32 index;
	uint32 references;
	uint32 transmit_active;
	uint32 state_pending;
	uint32 transmit_threads[NTHREAD];
	uint32 *transmit_cpus;
	struct net_device *next_state;
	uint8 registered;
	uint8 up;
	uint8 carrier;
	uint8 queue_stopped;
	uint8 lifecycle_transition;
	uint8 stop_pending;
	uint8 state_queued;
	uint8 loopback;
	struct device *parent;
	const struct net_device_operations *operations;
	void *private;
	struct net_device_stats stats;
};

typedef void (*net_receive_t)(struct net_packet *packet, void *argument);
typedef void (*net_state_t)(struct net_device *device, void *argument);

void net_device_init(void);
int net_device_register(struct net_device *device);
int net_device_unregister(struct net_device *device);
/* Lookup results remain registered until the matching put. */
struct net_device *net_device_get(uint32 index);
struct net_device *net_device_first(void);
void net_device_put(struct net_device *device);
int net_device_open(struct net_device *device);
void net_device_close(struct net_device *device);
void net_device_set_carrier(struct net_device *device, int carrier);
int net_device_carrier_ok(struct net_device *device);
void netif_stop_queue(struct net_device *device);
void netif_wake_queue(struct net_device *device);
int netif_queue_stopped(struct net_device *device);
int net_device_xmit(struct net_device *device,
		    struct net_packet *packet);
void net_device_rx_drop(struct net_device *device);
void net_device_get_stats(struct net_device *device,
			  struct net_device_stats *stats);
int net_receive_register(net_receive_t receive, void *argument);
void net_receive_unregister(net_receive_t receive, void *argument);
int net_state_register(net_state_t state, void *argument);
void net_state_unregister(net_state_t state, void *argument);
void netif_receive(struct net_packet *packet);

struct net_packet *net_packet_alloc(uint32 capacity);
struct net_packet *net_packet_get(struct net_packet *packet);
void net_packet_put(struct net_packet *packet);
uint32 net_packet_pool_available(void);

int net_core_selftest(void);
int net_loopback_init(void);

int virtio_net_init(void);

#endif
