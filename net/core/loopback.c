#include <mystring.h>
#include <netdevice.h>

static struct net_device loopback;

static int loopback_open(struct net_device *device)
{
	(void)device;
	return 0;
}

static void loopback_stop(struct net_device *device)
{
	(void)device;
}

static int loopback_xmit(struct net_device *device,
			 struct net_packet *packet)
{
	packet->device = device;
	netif_receive(packet);
	return 0;
}

static const struct net_device_operations loopback_operations = {
	.open = loopback_open,
	.stop = loopback_stop,
	.start_xmit = loopback_xmit,
};

int net_loopback_init(void)
{
	memset(&loopback, 0, sizeof(loopback));
	safe_strncpy(loopback.name, "lo", sizeof(loopback.name));
	loopback.mtu = NET_ETH_MTU;
	loopback.loopback = 1;
	loopback.operations = &loopback_operations;
	if (net_device_register(&loopback) < 0 ||
	    net_device_open(&loopback) < 0)
		return -1;
	net_device_set_carrier(&loopback, 1);
	return 0;
}
