/*
 * Caffeinix network-device core interface.
 *
 * Defines the driver/stack boundary, fixed packet-pool ownership rules, and
 * device lifecycle serialization.  Drivers never include protocol-stack
 * headers; callers own a packet until a successful transmit or receive handoff.
 */
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

/**
 * net_device_init() - Initialize the global device and packet registries
 *
 * Context: Early process context, once before drivers register devices.
 */
void net_device_init(void);

/**
 * net_device_register() - Publish a driver-owned Ethernet device
 * @device: Zeroed or private driver object with operations and valid MTU.
 *
 * Initializes core-owned fields and assigns a stable index and name.  The
 * caller retains ownership and must not free @device until unregister returns.
 *
 * Context: Process context; may allocate memory and sleep.
 * Return: 0 on success or -1 for invalid state, duplicate name, or exhaustion.
 */
int net_device_register(struct net_device *device);

/**
 * net_device_unregister() - Withdraw a device and drain core callbacks
 * @device: Previously registered driver-owned device.
 *
 * Waits for borrowed references, lifecycle transitions, and pending state
 * notifications while the device is still visible to lookup. Once those are
 * gone, removes it from lookup, marks it down, drains active transmissions,
 * and calls ->stop if needed before the final state notification. The caller
 * must release its own borrowed references first. State/receive/transmit
 * callbacks must not recursively tear down a device they keep active.
 *
 * Context: Process context; may sleep. No network or device lock may be held.
 * Return: 0 on success or -1 for an invalid device or recursive teardown.
 */
int net_device_unregister(struct net_device *device);

/**
 * net_device_get() - Find and pin a registered device by index
 * @index: Nonzero core-assigned device index.
 *
 * Return: A reference requiring net_device_put(), or NULL when absent.
 */
struct net_device *net_device_get(uint32 index);

/**
 * net_device_first() - Pin the first registered device
 *
 * Return: A reference requiring net_device_put(), or NULL when none exists.
 */
struct net_device *net_device_first(void);

/**
 * net_device_put() - Release a reference obtained from the core
 * @device: Referenced device, or NULL.
 *
 * Wakes unregister waiters after the final reference is released.
 *
 * Context: Any non-interrupt context; does not sleep.
 */
void net_device_put(struct net_device *device);

/**
 * net_device_open() - Start a registered device
 * @device: Registered device.
 *
 * Serializes with close and unregister, then invokes the driver's optional
 * ->open callback without the core locks held.
 *
 * Context: Process context; may sleep.
 * Return: 0 on success, the driver error, or -1 for invalid lifecycle state.
 */
int net_device_open(struct net_device *device);

/**
 * net_device_close() - Stop a device and prevent new transmissions
 * @device: Registered device, or NULL.
 *
 * Waits for competing transmit callbacks.  A close issued from the active
 * transmit callback is completed by its return path to avoid self-deadlock.
 *
 * Context: Process context; may sleep.
 */
void net_device_close(struct net_device *device);

/**
 * net_device_set_carrier() - Report physical link state
 * @device: Registered device, or NULL.
 * @carrier: Nonzero for link up, zero for link down.
 *
 * State callbacks are deferred when necessary so a driver may release its
 * private work after the notification path unwinds.
 *
 * Context: Sleepable process or deferred-work context; may sleep while a
 * state callback is serialized.  It must not be called from hard IRQ context.
 */
void net_device_set_carrier(struct net_device *device, int carrier);

/**
 * net_device_carrier_ok() - Read the last reported carrier state
 * @device: Device to inspect, or NULL.
 *
 * Return: Nonzero only for a device whose carrier is up.
 */
int net_device_carrier_ok(struct net_device *device);

/**
 * netif_stop_queue() - Refuse new driver transmissions
 * @device: Registered device, or NULL.
 *
 * Context: Any sleepable context; does not sleep.
 */
void netif_stop_queue(struct net_device *device);
/**
 * netif_wake_queue() - Permit new transmissions on an open device
 * @device: Registered device, or NULL.
 *
 * Context: Any sleepable context; does not sleep.
 */
void netif_wake_queue(struct net_device *device);
/**
 * netif_queue_stopped() - Read transmit backpressure state
 * @device: Device to inspect, or NULL.
 *
 * Context: Any sleepable context.
 *
 * Return: Nonzero when transmission is barred.
 */
int netif_queue_stopped(struct net_device *device);

/**
 * net_device_xmit() - Hand one Ethernet frame to a device driver
 * @device: Open, carrier-up destination device.
 * @packet: One referenced packet with a valid Ethernet frame.
 *
 * A successful return transfers the packet reference to ->start_xmit().  On
 * error, including %NETDEV_TX_BUSY, the caller still owns it and may retry.
 *
 * Context: Process or deferred-work context; drivers may impose their own
 * sleeping rules, but core locks are not held across ->start_xmit().
 * Return: 0, %NETDEV_TX_BUSY, or a negative driver/core error.
 */
int net_device_xmit(struct net_device *device,
		    struct net_packet *packet);

/**
 * net_device_rx_drop() - Account for one discarded receive frame
 * @device: Device receiving the drop, or NULL.
 *
 * Context: Any context that can use relaxed atomic accounting; does not sleep.
 */
void net_device_rx_drop(struct net_device *device);

/**
 * net_device_get_stats() - Take a relaxed snapshot of device counters
 * @device: Device to sample.
 * @stats: Caller-owned destination; must be non-NULL.
 */
void net_device_get_stats(struct net_device *device,
			  struct net_device_stats *stats);

/**
 * net_receive_register() - Install the sole stack-facing receive callback
 * @receive: Callback that consumes every delivered packet reference.
 * @argument: Opaque callback argument.
 *
 * Context: Process context; does not sleep.
 * Return: 0 on success or -1 when another callback is installed or draining.
 */
int net_receive_register(net_receive_t receive, void *argument);

/**
 * net_receive_unregister() - Remove and drain a receive callback
 * @receive: Callback identity passed to net_receive_register().
 * @argument: Matching opaque argument.
 *
 * Context: Process context; may sleep.  It can be called by the callback;
 * nested delivery is drained without waiting on the current invocation.
 */
void net_receive_unregister(net_receive_t receive, void *argument);

/**
 * net_state_register() - Install the sole device-state callback
 * @state: Callback invoked after administrative or carrier transitions.
 * @argument: Opaque callback argument.
 *
 * Return: 0 on success or -1 when the slot is occupied or draining.
 */
int net_state_register(net_state_t state, void *argument);
/**
 * net_state_unregister() - Remove and drain a device-state callback
 * @state: Callback identity passed at registration.
 * @argument: Matching opaque argument.
 *
 * Context: Process context; may sleep.
 */
void net_state_unregister(net_state_t state, void *argument);

/**
 * netif_receive() - Consume a validated frame from a driver
 * @packet: Referenced packet whose device field names its source.
 *
 * The function always consumes the reference: it invokes the registered
 * callback or releases the packet after validation or teardown failure.
 *
 * Context: Sleepable process or deferred-work context.  Loopback calls this
 * synchronously from transmit context; the callback may sleep, so hard IRQ
 * handlers must defer delivery.
 */
void netif_receive(struct net_packet *packet);

/**
 * net_packet_alloc() - Allocate a packet with one reference
 * @capacity: Usable payload capacity from 1 through %NET_PACKET_SIZE.
 *
 * Return: Packet owned by the caller, or NULL when the fixed pool is empty.
 */
struct net_packet *net_packet_alloc(uint32 capacity);
/**
 * net_packet_get() - Acquire another packet reference
 * @packet: Existing pooled packet, or NULL.
 *
 * Context: Any non-hard-IRQ context.
 *
 * Return: @packet or NULL on invalid state.
 */
struct net_packet *net_packet_get(struct net_packet *packet);
/**
 * net_packet_put() - Release one packet reference
 * @packet: Referenced packet, or NULL.
 *
 * Context: Any non-hard-IRQ context; final release returns it to the pool.
 */
void net_packet_put(struct net_packet *packet);
/**
 * net_packet_pool_available() - Read a transient free-packet count
 *
 * Context: Any non-hard-IRQ context.
 *
 * Return: Snapshot of available packets.
 */
uint32 net_packet_pool_available(void);

/**
 * net_core_selftest() - Exercise device lifecycle and ownership invariants
 *
 * Context: Boot selftest context; may sleep.
 *
 * Return: 0 on success or -1.
 */
int net_core_selftest(void);
/**
 * net_loopback_init() - Register and activate the software loopback device
 *
 * Context: Boot process context; may sleep.
 *
 * Return: 0 on success or -1.
 */
int net_loopback_init(void);

int virtio_net_init(void);

#endif
