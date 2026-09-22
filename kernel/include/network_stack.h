/*
 * Protocol-stack boundary for Caffeinix network devices.
 *
 * The implementation currently adapts net_device packets to lwIP.  These
 * declarations deliberately expose snapshots and policy queries, not lwIP
 * objects, so callers remain independent of the selected protocol stack.
 */
#ifndef __CAFFEINIX_KERNEL_NETWORK_STACK_H
#define __CAFFEINIX_KERNEL_NETWORK_STACK_H

#include <netdevice.h>
#include <typedefs.h>

struct network_interface_snapshot {
	char name[NET_DEVICE_NAME_SIZE];
	uint8 address[NET_ETH_ADDRESS_LENGTH];
	uint32 index;
	uint32 mtu;
	uint32 ipv4_address;
	uint32 ipv4_netmask;
	uint32 ipv4_gateway;
	uint8 up;
	uint8 running;
	uint8 loopback;
	uint8 broadcast;
};

/**
 * network_stack_init() - Start the protocol stack asynchronously
 *
 * Context: Early process context, once after net_device_init().
 */
void network_stack_init(void);

/**
 * network_stack_address_is_broadcast() - Test IPv4 broadcast reachability
 * @address: IPv4 address in lwIP network byte order.
 *
 * Return: Nonzero when @address is broadcast on any attached interface.
 */
int network_stack_address_is_broadcast(uint32 address);

/**
 * network_stack_snapshot_interfaces() - Copy protocol-visible interfaces
 * @snapshots: Caller-owned array receiving at most @capacity snapshots.
 * @capacity: Number of entries in @snapshots; must be nonzero.
 * @count: Output number copied; must be non-NULL.
 *
 * Runs the snapshot operation in the protocol-stack context and pins each
 * device while reading it, so an unregister cannot expose freed driver data.
 *
 * Context: Process context; may wait for the stack thread.
 * Return: 0 on success or -1 before the stack is ready or on callback failure.
 */
int network_stack_snapshot_interfaces(
	struct network_interface_snapshot *snapshots, uint32 capacity,
	uint32 *count);

#endif
