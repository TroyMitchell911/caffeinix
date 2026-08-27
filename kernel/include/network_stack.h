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

void network_stack_init(void);
int network_stack_address_is_broadcast(uint32 address);
int network_stack_snapshot_interfaces(
	struct network_interface_snapshot *snapshots, uint32 capacity,
	uint32 *count);

#endif
