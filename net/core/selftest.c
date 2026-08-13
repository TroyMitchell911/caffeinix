#include <mystring.h>
#include <netdevice.h>

struct net_selftest_state {
	uint32 opened;
	uint32 stopped;
	uint32 transmitted;
	uint32 received;
	uint32 state_changes;
	uint32 receive_unregistered;
	uint32 receive_recursive;
	uint32 receive_depth;
	uint32 receive_depth_max;
	uint32 receive_pinned;
	uint32 state_unregistered;
	uint32 state_recursive;
	uint32 state_depth;
	uint32 state_depth_max;
	uint32 receive_closed;
	uint32 state_closed;
	uint32 state_opened;
	uint32 device_unregistered;
	uint32 removed_closed;
	uint32 lifecycle_failures;
	int xmit_unregister_status;
	int receive_unregister_status;
	int state_unregister_status;
	struct net_device *state_recursive_device;
};

static struct net_selftest_state selftest;

static int selftest_open(struct net_device *device)
{
	(void)device;
	selftest.opened++;
	return 0;
}

static int selftest_open_failed(struct net_device *device)
{
	(void)device;
	return -1;
}

static void selftest_stop(struct net_device *device)
{
	(void)device;
	selftest.stopped++;
}

static int selftest_xmit(struct net_device *device,
			 struct net_packet *packet)
{
	(void)device;
	selftest.transmitted++;
	net_packet_put(packet);
	return 0;
}

static int selftest_receive_xmit(struct net_device *device,
				 struct net_packet *packet)
{
	packet->device = device;
	netif_receive(packet);
	return 0;
}

static void selftest_receive(struct net_packet *packet, void *argument)
{
	if (argument == &selftest && packet && packet->length == 64)
		selftest.received++;
	net_packet_put(packet);
}

static void selftest_state(struct net_device *device, void *argument)
{
	if (argument == &selftest && device)
		selftest.state_changes++;
}

static void selftest_receive_unregister(struct net_packet *packet,
					void *argument)
{
	if (argument == &selftest && packet)
		selftest.receive_unregistered++;
	net_receive_unregister(selftest_receive_unregister, argument);
	net_packet_put(packet);
}

static void selftest_receive_recursive_unregister(struct net_packet *packet,
						   void *argument)
{
	struct net_selftest_state *state = argument;
	struct net_packet *nested;

	if (state != &selftest || !packet || !packet->device) {
		net_packet_put(packet);
		return;
	}
	state->receive_depth++;
	if (state->receive_depth > state->receive_depth_max)
		state->receive_depth_max = state->receive_depth;
	state->receive_recursive++;
	if (state->receive_recursive == 1) {
		nested = net_packet_alloc(64);
		if (!nested)
			state->lifecycle_failures++;
		else {
			nested->device = packet->device;
			nested->length = 64;
			netif_receive(nested);
		}
		spinlock_acquire(&packet->device->lock);
		if (packet->device->references == 2)
			state->receive_pinned++;
		else
			state->lifecycle_failures++;
		spinlock_release(&packet->device->lock);
		state->receive_unregister_status =
			net_device_unregister(packet->device);
	} else {
		net_receive_unregister(
			selftest_receive_recursive_unregister, argument);
	}
	state->receive_depth--;
	net_packet_put(packet);
}

static void selftest_state_recursive_unregister(struct net_device *device,
						 void *argument)
{
	struct net_selftest_state *state = argument;

	if (state != &selftest || !device ||
	    !state->state_recursive_device)
		return;
	state->state_depth++;
	if (state->state_depth > state->state_depth_max)
		state->state_depth_max = state->state_depth;
	state->state_recursive++;
	if (state->state_recursive == 1)
		net_device_set_carrier(state->state_recursive_device, 1);
	else
		net_state_unregister(selftest_state_recursive_unregister,
				     argument);
	state->state_depth--;
}

static void selftest_state_unregister(struct net_device *device,
				      void *argument)
{
	if (argument == &selftest && device)
		selftest.state_unregistered++;
	net_state_unregister(selftest_state_unregister, argument);
}

static void selftest_receive_close(struct net_packet *packet,
				   void *argument)
{
	if (argument == &selftest && packet && packet->device) {
		selftest.receive_closed++;
		net_device_close(packet->device);
	}
	net_packet_put(packet);
}

static void selftest_state_close(struct net_device *device, void *argument)
{
	if (argument == &selftest && device && device->up) {
		selftest.state_closed++;
		net_device_close(device);
	}
}

static void selftest_state_open(struct net_device *device, void *argument)
{
	if (argument == &selftest && device && device->registered &&
	    !device->up) {
		selftest.state_opened++;
		if (net_device_open(device))
			selftest.lifecycle_failures++;
	}
}

static void selftest_state_device_unregister(struct net_device *device,
					     void *argument)
{
	if (argument == &selftest && device && device->registered &&
	    device->up) {
		selftest.device_unregistered++;
		selftest.state_unregister_status =
			net_device_unregister(device);
		if (selftest.state_unregister_status >= 0)
			selftest.lifecycle_failures++;
	}
}

static void selftest_state_removed_close(struct net_device *device,
					 void *argument)
{
	if (argument == &selftest && device) {
		selftest.removed_closed++;
		net_device_close(device);
	}
}

static int selftest_unregister_xmit(struct net_device *device,
				    struct net_packet *packet)
{
	selftest.xmit_unregister_status = net_device_unregister(device);
	net_packet_put(packet);
	return 0;
}

int net_core_selftest(void)
{
	static const struct net_device_operations operations = {
		.open = selftest_open,
		.stop = selftest_stop,
		.start_xmit = selftest_xmit,
	};
	static const struct net_device_operations failed_operations = {
		.open = selftest_open_failed,
		.start_xmit = selftest_xmit,
	};
	static const struct net_device_operations receive_operations = {
		.open = selftest_open,
		.stop = selftest_stop,
		.start_xmit = selftest_receive_xmit,
	};
	static const struct net_device_operations unregister_operations = {
		.open = selftest_open,
		.stop = selftest_stop,
		.start_xmit = selftest_unregister_xmit,
	};
	struct net_device device = {
		.mtu = NET_ETH_MTU,
		.operations = &operations,
	};
	struct net_device duplicate = {
		.mtu = NET_ETH_MTU,
		.operations = &operations,
	};
	struct net_device failed = {
		.mtu = NET_ETH_MTU,
		.operations = &failed_operations,
	};
	struct net_device receive_close = {
		.mtu = NET_ETH_MTU,
		.operations = &receive_operations,
	};
	struct net_device state_close = {
		.mtu = NET_ETH_MTU,
		.operations = &operations,
	};
	struct net_device state_unregister = {
		.mtu = NET_ETH_MTU,
		.operations = &operations,
	};
	struct net_device state_recursive = {
		.mtu = NET_ETH_MTU,
		.operations = &operations,
	};
	struct net_device xmit_unregister = {
		.mtu = NET_ETH_MTU,
		.operations = &unregister_operations,
	};
	struct net_device removed_close = {
		.mtu = NET_ETH_MTU,
		.operations = &operations,
	};
	struct net_device lookup_device = {
		.mtu = NET_ETH_MTU,
		.operations = &operations,
	};
	struct net_device *by_index = 0, *first = 0;
	struct net_device_stats stats;
	struct net_packet *packet;
	uint32 available = net_packet_pool_available();
	int result = -1;

	memset(&selftest, 0, sizeof(selftest));
	safe_strncpy(device.name, "net-test", sizeof(device.name));
	safe_strncpy(duplicate.name, "net-test", sizeof(duplicate.name));
	safe_strncpy(failed.name, "net-fail", sizeof(failed.name));
	safe_strncpy(receive_close.name, "net-close",
		     sizeof(receive_close.name));
	safe_strncpy(state_close.name, "net-state-close",
		     sizeof(state_close.name));
	safe_strncpy(state_unregister.name, "net-state-unreg",
		     sizeof(state_unregister.name));
	safe_strncpy(state_recursive.name, "net-state-rec",
		     sizeof(state_recursive.name));
	safe_strncpy(xmit_unregister.name, "net-xmit-unreg",
		     sizeof(xmit_unregister.name));
	safe_strncpy(removed_close.name, "net-removed-close",
		     sizeof(removed_close.name));
	safe_strncpy(lookup_device.name, "net-lookup",
		     sizeof(lookup_device.name));
	if (net_device_register(&device) ||
	    !net_device_register(&duplicate) ||
	    net_state_register(selftest_state, &selftest) ||
	    net_receive_register(selftest_receive, &selftest))
		goto out;
	if (net_device_open(&device))
		goto out;
	if (net_device_open(&device) || selftest.opened != 1)
		goto out;
	net_device_set_carrier(&device, 1);
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->length = NET_ETH_HEADER_LENGTH - 1;
	if (net_device_xmit(&device, packet) >= 0) {
		net_packet_put(packet);
		goto out;
	}
	net_packet_put(packet);
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->device = &device;
	packet->length = NET_ETH_HEADER_LENGTH - 1;
	netif_receive(packet);
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->length = 64;
	if (net_device_xmit(&device, packet)) {
		net_packet_put(packet);
		goto out;
	}
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->device = &device;
	packet->length = 64;
	netif_receive(packet);
	netif_stop_queue(&device);
	net_device_set_carrier(&device, 0);
	netif_wake_queue(&device);
	if (netif_queue_stopped(&device))
		goto out;
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->length = 64;
	if (net_device_xmit(&device, packet) != NETDEV_TX_BUSY) {
		net_packet_put(packet);
		goto out;
	}
	net_packet_put(packet);
	net_device_set_carrier(&device, 1);
	if (netif_queue_stopped(&device))
		goto out;
	net_device_get_stats(&device, &stats);
	if (stats.tx_packets != 1 || stats.tx_bytes != 64 ||
	    stats.rx_packets != 1 || stats.rx_bytes != 64 ||
	    stats.rx_dropped != 1 ||
	    selftest.opened != 1 || selftest.transmitted != 1 ||
	    selftest.received != 1 || selftest.state_changes < 2 ||
	    device.transmit_active)
		goto out;
	net_device_close(&device);
	if (device.up || selftest.stopped != 1)
		goto out;
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->length = 64;
	if (net_device_xmit(&device, packet) != NETDEV_TX_BUSY) {
		net_packet_put(packet);
		goto out;
	}
	net_packet_put(packet);
	if (net_device_open(&device) || selftest.opened != 2)
		goto out;
	if (net_device_register(&failed) || !net_device_open(&failed))
		goto out;
	net_device_unregister(&failed);
	net_receive_unregister(selftest_receive, &selftest);
	if (net_receive_register(selftest_receive_unregister, &selftest))
		goto out;
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->device = &device;
	packet->length = 64;
	netif_receive(packet);
	if (selftest.receive_unregistered != 1 ||
	    net_receive_register(selftest_receive, &selftest))
		goto out;
	net_receive_unregister(selftest_receive, &selftest);
	if (net_receive_register(selftest_receive_recursive_unregister,
				 &selftest))
		goto out;
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->device = &device;
	packet->length = 64;
	netif_receive(packet);
	if (selftest.receive_recursive != 2 || selftest.receive_depth ||
	    selftest.receive_depth_max != 1 || selftest.receive_pinned != 1 ||
	    selftest.receive_unregister_status >= 0 || device.references ||
	    selftest.lifecycle_failures ||
	    net_receive_register(selftest_receive, &selftest))
		goto out;
	net_state_unregister(selftest_state, &selftest);
	if (net_state_register(selftest_state_unregister, &selftest))
		goto out;
	net_device_set_carrier(&device, 0);
	if (selftest.state_unregistered != 1 ||
	    net_state_register(selftest_state, &selftest))
		goto out;
	net_device_set_carrier(&device, 1);
	if (net_device_register(&state_recursive) ||
	    net_device_open(&state_recursive))
		goto out;
	selftest.state_recursive_device = &state_recursive;
	net_state_unregister(selftest_state, &selftest);
	if (net_state_register(selftest_state_recursive_unregister,
			       &selftest))
		goto out;
	net_device_set_carrier(&device, 0);
	if (selftest.state_recursive != 2 || selftest.state_depth ||
	    selftest.state_depth_max != 1 || selftest.lifecycle_failures ||
	    net_state_register(selftest_state, &selftest))
		goto out;
	net_device_set_carrier(&device, 1);
	net_receive_unregister(selftest_receive, &selftest);
	if (net_receive_register(selftest_receive_close, &selftest) ||
	    net_device_register(&receive_close) ||
	    net_device_open(&receive_close))
		goto out;
	net_device_set_carrier(&receive_close, 1);
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->length = 64;
	if (net_device_xmit(&receive_close, packet) ||
	    selftest.receive_closed != 1 || receive_close.up ||
	    receive_close.lifecycle_transition ||
	    receive_close.transmit_active || selftest.stopped != 2)
		goto out;
	net_receive_unregister(selftest_receive_close, &selftest);
	if (net_device_unregister(&receive_close))
		goto out;
	net_state_unregister(selftest_state, &selftest);
	if (net_state_register(selftest_state_close, &selftest) ||
	    net_device_register(&state_close) ||
	    net_device_open(&state_close) || state_close.up ||
	    selftest.state_closed != 1)
		goto out;
	net_state_unregister(selftest_state_close, &selftest);
	if (net_state_register(selftest_state_open, &selftest) ||
	    net_device_open(&state_close))
		goto out;
	net_device_close(&state_close);
	if (!state_close.up || selftest.state_opened != 1 ||
	    selftest.lifecycle_failures)
		goto out;
	if (net_device_unregister(&state_close))
		goto out;
	net_state_unregister(selftest_state_open, &selftest);
	if (net_state_register(selftest_state_device_unregister,
			       &selftest) ||
	    net_device_register(&state_unregister) ||
	    net_device_open(&state_unregister) ||
	    !state_unregister.registered || !state_unregister.up ||
	    selftest.device_unregistered != 1 ||
	    selftest.state_unregister_status >= 0 ||
	    selftest.lifecycle_failures)
		goto out;
	net_state_unregister(selftest_state_device_unregister, &selftest);
	if (net_device_unregister(&state_unregister))
		goto out;
	if (net_state_register(selftest_state, &selftest) ||
	    net_device_register(&xmit_unregister) ||
	    net_device_open(&xmit_unregister))
		goto out;
	net_device_set_carrier(&xmit_unregister, 1);
	packet = net_packet_alloc(64);
	if (!packet)
		goto out;
	packet->length = 64;
	selftest.xmit_unregister_status = 0;
	if (net_device_xmit(&xmit_unregister, packet) ||
	    selftest.xmit_unregister_status >= 0 ||
	    !xmit_unregister.registered || !xmit_unregister.up)
		goto out;
	if (net_device_unregister(&xmit_unregister))
		goto out;
	if (net_device_register(&lookup_device) ||
	    net_device_open(&lookup_device))
		goto out;
	by_index = net_device_get(lookup_device.index);
	first = net_device_first();
	if (by_index != &lookup_device || !first)
		goto out;
	net_device_put(first);
	first = 0;
	net_device_put(by_index);
	by_index = 0;
	if (lookup_device.references ||
	    net_device_unregister(&lookup_device))
		goto out;
	if (net_device_register(&removed_close) ||
	    net_device_open(&removed_close))
		goto out;
	net_state_unregister(selftest_state, &selftest);
	if (net_state_register(selftest_state_removed_close, &selftest) ||
	    net_device_unregister(&removed_close) ||
	    selftest.removed_closed != 1)
		goto out;
	result = 0;

out:
	if (first)
		net_device_put(first);
	if (by_index)
		net_device_put(by_index);
	net_receive_unregister(selftest_receive, &selftest);
	net_receive_unregister(selftest_receive_unregister, &selftest);
	net_receive_unregister(selftest_receive_recursive_unregister,
			       &selftest);
	net_receive_unregister(selftest_receive_close, &selftest);
	net_state_unregister(selftest_state, &selftest);
	net_state_unregister(selftest_state_unregister, &selftest);
	net_state_unregister(selftest_state_recursive_unregister, &selftest);
	net_state_unregister(selftest_state_close, &selftest);
	net_state_unregister(selftest_state_open, &selftest);
	net_state_unregister(selftest_state_device_unregister, &selftest);
	net_state_unregister(selftest_state_removed_close, &selftest);
	net_device_unregister(&device);
	if (failed.registered)
		net_device_unregister(&failed);
	if (duplicate.registered)
		net_device_unregister(&duplicate);
	if (receive_close.registered)
		net_device_unregister(&receive_close);
	if (state_close.registered)
		net_device_unregister(&state_close);
	if (state_unregister.registered)
		net_device_unregister(&state_unregister);
	if (state_recursive.registered)
		net_device_unregister(&state_recursive);
	if (xmit_unregister.registered)
		net_device_unregister(&xmit_unregister);
	if (removed_close.registered)
		net_device_unregister(&removed_close);
	if (lookup_device.registered)
		net_device_unregister(&lookup_device);
	if (net_packet_pool_available() != available)
		result = -1;
	return result;
}
