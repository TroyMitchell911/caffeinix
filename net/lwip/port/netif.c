#include <debug.h>
#include <lwip/dhcp.h>
#include <lwip/etharp.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/pbuf.h>
#include <lwip/tcpip.h>
#include <mystring.h>
#include <netdevice.h>
#include <network_stack.h>
#include <printk.h>
#include <scheduler.h>
#include <workqueue.h>

static struct {
	struct {
		struct netif interface;
		struct net_device *device;
		uint8 loopback;
	} adapters[NET_DEVICE_MAX];
	struct work_struct state_work[NET_DEVICE_MAX];
	uint32 count;
	struct netif *default_interface;
	thread_t tcpip_thread;
} lwip_network;

static typeof(lwip_network.adapters[0]) *lwip_find_adapter(
					struct net_device *device)
{
	uint32 index;

	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (__atomic_load_n(&lwip_network.adapters[index].device,
				    __ATOMIC_ACQUIRE) == device)
			return &lwip_network.adapters[index];
	}
	return 0;
}

static typeof(lwip_network.adapters[0]) *lwip_find_free_adapter(void)
{
	uint32 index;

	for (index = 0; index < NET_DEVICE_MAX; index++) {
		if (!__atomic_load_n(&lwip_network.adapters[index].device,
				     __ATOMIC_ACQUIRE))
			return &lwip_network.adapters[index];
	}
	return 0;
}

static struct netif *lwip_choose_default_interface(void)
{
	struct netif *fallback = 0;
	uint32 index;

	for (index = 0; index < NET_DEVICE_MAX; index++) {
		typeof(lwip_network.adapters[0]) *adapter =
			&lwip_network.adapters[index];

		if (!__atomic_load_n(&adapter->device, __ATOMIC_ACQUIRE))
			continue;
		if (!adapter->loopback)
			return &adapter->interface;
		fallback = &adapter->interface;
	}
	return fallback;
}

static err_t lwip_send_packet(struct net_device *device,
			      struct pbuf *pbuf)
{
	struct net_packet *packet;
	int status;

	if (!device || !pbuf || !pbuf->tot_len ||
	    pbuf->tot_len > device->mtu + NET_ETH_HEADER_LENGTH)
		return ERR_ARG;
	packet = net_packet_alloc(pbuf->tot_len);
	if (!packet)
		return ERR_MEM;
	if (pbuf_copy_partial(pbuf, packet->data, pbuf->tot_len, 0) !=
	    pbuf->tot_len) {
		net_packet_put(packet);
		return ERR_BUF;
	}
	packet->length = pbuf->tot_len;
	status = net_device_xmit(device, packet);
	if (status < 0) {
		net_packet_put(packet);
		return status == NETDEV_TX_BUSY ? ERR_MEM : ERR_IF;
	}
	return ERR_OK;
}

static err_t lwip_link_output(struct netif *interface, struct pbuf *pbuf)
{
	typeof(lwip_network.adapters[0]) *adapter = interface->state;

	return adapter ? lwip_send_packet(adapter->device, pbuf) : ERR_ARG;
}

static err_t lwip_loop_output(struct netif *interface, struct pbuf *pbuf,
			      const ip4_addr_t *address)
{
	typeof(lwip_network.adapters[0]) *adapter = interface->state;

	(void)address;
	return adapter ? lwip_send_packet(adapter->device, pbuf) : ERR_ARG;
}

static err_t lwip_netif_init(struct netif *interface)
{
	typeof(lwip_network.adapters[0]) *adapter = interface->state;
	struct net_device *device = adapter ? adapter->device : 0;

	if (!device)
		return ERR_ARG;
	interface->hostname = "caffeinix";
	interface->mtu = device->mtu;
	if (device->loopback) {
		interface->name[0] = 'l';
		interface->name[1] = 'o';
		interface->output = lwip_loop_output;
		return ERR_OK;
	}
	interface->name[0] = 'e';
	interface->name[1] = 'n';
	interface->output = etharp_output;
	interface->linkoutput = lwip_link_output;
	interface->hwaddr_len = NET_ETH_ADDRESS_LENGTH;
	memmove(interface->hwaddr, device->address,
		NET_ETH_ADDRESS_LENGTH);
	interface->flags = NETIF_FLAG_BROADCAST | NETIF_FLAG_ETHARP |
			   NETIF_FLAG_ETHERNET;
	return ERR_OK;
}

static void lwip_receive(struct net_packet *packet, void *argument)
{
	typeof(lwip_network.adapters[0]) *adapter;
	struct netif *interface;
	struct pbuf *pbuf;

	(void)argument;
	adapter = packet ? lwip_find_adapter(packet->device) : 0;
	interface = adapter ? &adapter->interface : 0;
	if (!packet || !interface ||
	    !netif_is_up(interface) || !netif_is_link_up(interface)) {
		net_packet_put(packet);
		return;
	}
	pbuf = pbuf_alloc(PBUF_RAW, packet->length, PBUF_POOL);
	if (!pbuf || pbuf_take(pbuf, packet->data, packet->length) != ERR_OK) {
		if (pbuf)
			pbuf_free(pbuf);
		net_packet_put(packet);
		net_device_rx_drop(adapter->device);
		return;
	}
	net_packet_put(packet);
	if (interface->input(pbuf, interface) != ERR_OK)
		pbuf_free(pbuf);
}

static void lwip_apply_device_state(void *argument)
{
	typeof(lwip_network.adapters[0]) *adapter = argument;
	struct net_device *device = __atomic_load_n(&adapter->device,
						    __ATOMIC_ACQUIRE);
	struct netif *interface;

	if (!device)
		return;
	interface = &adapter->interface;
	if (device->up && net_device_carrier_ok(device))
		netif_set_link_up(interface);
	else
		netif_set_link_down(interface);
}

static void lwip_set_default_interface(struct netif *interface)
{
	if (!interface)
		return;
	if (cur_thread() == lwip_network.tcpip_thread)
		netif_set_default(interface);
	else if (netifapi_netif_set_default(interface) != ERR_OK)
		PANIC("set lwIP default interface");
}

static void lwip_detach_device(struct net_device *device)
{
	typeof(lwip_network.adapters[0]) *adapter =
		lwip_find_adapter(device);
	struct netif *interface;
	int in_tcpip_thread;

	if (!adapter)
		return;
	interface = &adapter->interface;
	in_tcpip_thread = cur_thread() == lwip_network.tcpip_thread;
	if (!adapter->loopback) {
		if (in_tcpip_thread)
			dhcp_stop(interface);
		else if (netifapi_dhcp_stop(interface) != ERR_OK)
			PANIC("stop lwIP DHCP");
	}
	if (in_tcpip_thread)
		netif_remove(interface);
	else if (netifapi_netif_remove(interface) != ERR_OK)
		PANIC("remove lwIP interface");
	if (lwip_network.default_interface == interface)
		lwip_network.default_interface = 0;
	__atomic_store_n(&adapter->device, 0, __ATOMIC_RELEASE);
	if (lwip_network.count)
		lwip_network.count--;
	if (!lwip_network.default_interface) {
		lwip_network.default_interface =
			lwip_choose_default_interface();
		lwip_set_default_interface(lwip_network.default_interface);
	}
	pr_notice("lwIP: detached %s", device->name);
}

static void lwip_device_state_work(struct work_struct *work)
{
	typeof(lwip_network.adapters[0]) *adapter;
	uint32 index = work - lwip_network.state_work;
	err_t result;

	if (index >= NET_DEVICE_MAX)
		PANIC("invalid lwIP state work");
	adapter = &lwip_network.adapters[index];
	if (!__atomic_load_n(&adapter->device, __ATOMIC_ACQUIRE))
		return;
	do {
		result = tcpip_callback(lwip_apply_device_state, adapter);
		if (result == ERR_MEM)
			yield();
	} while (result == ERR_MEM);
	if (result != ERR_OK)
		PANIC("queue lwIP link state");
}

static void lwip_device_state(struct net_device *device, void *argument)
{
	typeof(lwip_network.adapters[0]) *adapter;
	uint32 index;

	(void)argument;
	adapter = lwip_find_adapter(device);
	if (!adapter)
		return;
	if (!device->registered) {
		lwip_detach_device(device);
		return;
	}
	if (cur_thread() == lwip_network.tcpip_thread) {
		lwip_apply_device_state(adapter);
		return;
	}
	index = adapter - lwip_network.adapters;
	if (schedule_work(&lwip_network.state_work[index]) < 0)
		PANIC("schedule lwIP link state");
}

static void lwip_status_changed(struct netif *interface)
{
	typeof(lwip_network.adapters[0]) *adapter = interface->state;
	const ip4_addr_t *address = netif_ip4_addr(interface);
	uint32 value;

	if (!adapter || !adapter->device || adapter->loopback ||
	    !netif_is_up(interface) || ip4_addr_isany_val(*address))
		return;
	value = lwip_ntohl(ip4_addr_get_u32(address));
	pr_info("%s: IPv4 %d.%d.%d.%d", adapter->device->name,
		(value >> 24) & 0xff, (value >> 16) & 0xff,
		(value >> 8) & 0xff, value & 0xff);
}

static int lwip_attach_device(struct net_device *device)
{
	typeof(lwip_network.adapters[0]) *adapter;
	ip4_addr_t address, netmask, gateway;
	struct netif *interface;

	if (!device || lwip_network.count >= NET_DEVICE_MAX)
		return -1;
	adapter = lwip_find_free_adapter();
	if (!adapter)
		return -1;
	memset(adapter, 0, sizeof(*adapter));
	adapter->device = device;
	adapter->loopback = device->loopback;
	if (device->loopback) {
		IP4_ADDR(&address, 127, 0, 0, 1);
		IP4_ADDR(&netmask, 255, 0, 0, 0);
		IP4_ADDR(&gateway, 127, 0, 0, 1);
	} else {
		ip4_addr_set_zero(&address);
		ip4_addr_set_zero(&netmask);
		ip4_addr_set_zero(&gateway);
	}
	interface = netif_add(&adapter->interface, &address, &netmask,
			      &gateway, adapter, lwip_netif_init,
			      tcpip_input);
	if (!interface) {
		adapter->device = 0;
		return -1;
	}
	lwip_network.count++;
	netif_set_status_callback(interface, lwip_status_changed);
	netif_set_up(interface);
	lwip_apply_device_state(adapter);
	pr_info("lwIP: attached %s", device->name);
	if (device->loopback)
		return 0;
	if (!lwip_network.default_interface) {
		lwip_network.default_interface = interface;
		netif_set_default(interface);
	}
	if (dhcp_start(interface) != ERR_OK)
		pr_warn("lwIP: DHCP start failed on %s", device->name);
	return 0;
}

static void lwip_tcpip_ready(void *argument)
{
	struct net_device *device;
	int external_devices = 0;
	uint32 index;

	(void)argument;
	lwip_network.tcpip_thread = cur_thread();
	for (index = 0; index < NET_DEVICE_MAX; index++)
		work_init(&lwip_network.state_work[index],
			  lwip_device_state_work);
	if (net_receive_register(lwip_receive, &lwip_network) < 0) {
		pr_err("lwIP: cannot register receive path");
		return;
	}
	if (net_state_register(lwip_device_state, &lwip_network) < 0) {
		net_receive_unregister(lwip_receive, &lwip_network);
		pr_err("lwIP: cannot register device state");
		return;
	}
	for (index = 1; index < NET_DEVICE_MAX; index++) {
		device = net_device_get(index);
		if (device) {
			if (lwip_attach_device(device) < 0) {
				pr_warn("lwIP: cannot add %s", device->name);
			} else if (!device->loopback) {
				external_devices++;
			}
			net_device_put(device);
		}
	}
	if (!lwip_network.count)
		pr_notice("lwIP: no network device");
	else if (!external_devices)
		pr_notice("lwIP: no external network device");
	else if (!lwip_network.default_interface) {
		lwip_network.default_interface =
			lwip_choose_default_interface();
		netif_set_default(lwip_network.default_interface);
	}
}

void network_stack_init(void)
{
	memset(&lwip_network, 0, sizeof(lwip_network));
	tcpip_init(lwip_tcpip_ready, 0);
}

struct lwip_interface_snapshot_request {
	struct network_interface_snapshot *snapshots;
	uint32 capacity;
	uint32 count;
};

static void lwip_snapshot_interfaces(void *argument)
{
	struct lwip_interface_snapshot_request *request = argument;
	uint32 adapter_index;

	for (adapter_index = 0; adapter_index < NET_DEVICE_MAX;
	     adapter_index++) {
		typeof(lwip_network.adapters[0]) *adapter =
			&lwip_network.adapters[adapter_index];
		struct network_interface_snapshot *snapshot;
		struct net_device *device, *held;
		struct netif *interface;
		uint32 device_index;

		device = __atomic_load_n(&adapter->device, __ATOMIC_ACQUIRE);
		if (!device || request->count >= request->capacity)
			continue;
		device_index = __atomic_load_n(&device->index,
					       __ATOMIC_ACQUIRE);
		held = net_device_get(device_index);
		if (held != device) {
			if (held)
				net_device_put(held);
			continue;
		}
		interface = &adapter->interface;
		snapshot = &request->snapshots[request->count];
		memset(snapshot, 0, sizeof(*snapshot));
		spinlock_acquire(&device->lock);
		safe_strncpy(snapshot->name, device->name,
			     sizeof(snapshot->name));
		memmove(snapshot->address, device->address,
			NET_ETH_ADDRESS_LENGTH);
		snapshot->index = device->index;
		snapshot->mtu = device->mtu;
		snapshot->up = device->up && netif_is_up(interface);
		snapshot->running = device->carrier &&
			netif_is_link_up(interface);
		snapshot->loopback = device->loopback;
		snapshot->broadcast = !device->loopback;
		spinlock_release(&device->lock);
		snapshot->ipv4_address =
			ip4_addr_get_u32(netif_ip4_addr(interface));
		snapshot->ipv4_netmask =
			ip4_addr_get_u32(netif_ip4_netmask(interface));
		snapshot->ipv4_gateway =
			ip4_addr_get_u32(netif_ip4_gw(interface));
		request->count++;
		net_device_put(held);
	}
}

int network_stack_snapshot_interfaces(
	struct network_interface_snapshot *snapshots, uint32 capacity,
	uint32 *count)
{
	struct lwip_interface_snapshot_request request = {
		.snapshots = snapshots,
		.capacity = capacity,
	};

	if (!snapshots || !capacity || !count ||
	    !__atomic_load_n(&lwip_network.tcpip_thread, __ATOMIC_ACQUIRE))
		return -1;
	if (cur_thread() == lwip_network.tcpip_thread)
		lwip_snapshot_interfaces(&request);
	else if (tcpip_callback_wait(lwip_snapshot_interfaces, &request) !=
		 ERR_OK)
		return -1;
	*count = request.count;
	return 0;
}

int network_stack_address_is_broadcast(uint32 address)
{
	ip4_addr_t destination;
	uint32 index;

	ip4_addr_set_u32(&destination, address);
	for (index = 0; index < NET_DEVICE_MAX; index++) {
		typeof(lwip_network.adapters[0]) *adapter =
			&lwip_network.adapters[index];

		if (!__atomic_load_n(&adapter->device, __ATOMIC_ACQUIRE))
			continue;
		if (ip4_addr_isbroadcast(&destination, &adapter->interface))
			return 1;
	}
	return 0;
}
