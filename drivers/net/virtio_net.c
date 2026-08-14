#include <debug.h>
#include <mystring.h>
#include <netdevice.h>
#include <palloc.h>
#include <printk.h>
#include <virtio.h>
#include <virtio_ring.h>
#include <workqueue.h>

#define VIRTIO_NET_F_MAC 5
#define VIRTIO_NET_F_STATUS 16
#define VIRTIO_NET_S_LINK_UP 1
#define VIRTIO_NET_CONFIG_STATUS 6

#define VIRTIO_NET_RX_QUEUE 0
#define VIRTIO_NET_TX_QUEUE 1
#define VIRTIO_NET_QUEUE_COUNT 2
#define VIRTIO_NET_RX_BUFFERS 32
#define VIRTIO_NET_RX_BUDGET 32

struct virtio_net_header {
	uint8 flags;
	uint8 gso_type;
	uint16 header_length;
	uint16 gso_size;
	uint16 checksum_start;
	uint16 checksum_offset;
	uint16 num_buffers;
};

/* VERSION_1 devices always include num_buffers in the network header. */
_Static_assert(sizeof(struct virtio_net_header) == 12,
	       "VirtIO net header layout changed");

struct virtio_net_rx_buffer {
	void *page;
};

struct virtio_net_tx_request {
	struct virtio_net_header header;
	struct net_packet *packet;
};

struct virtio_net {
	struct virtio_device *virtio;
	struct virtqueue *rx_queue;
	struct virtqueue *tx_queue;
	struct net_device netdev;
	struct work_struct poll_work;
	uint32 rx_buffers;
	uint8 rx_enabled;
};

static void virtio_net_update_carrier(struct virtio_net *network)
{
	uint16 status = VIRTIO_NET_S_LINK_UP;

	if (virtio_has_feature(network->virtio, VIRTIO_NET_F_STATUS))
		network->virtio->config->get_config(network->virtio,
			VIRTIO_NET_CONFIG_STATUS, &status, sizeof(status));
	net_device_set_carrier(&network->netdev,
			       status & VIRTIO_NET_S_LINK_UP);
}

static int virtio_net_post_rx(struct virtio_net *network,
			      struct virtio_net_rx_buffer *receive)
{
	struct virtio_buffer buffer = {
		.address = receive->page,
		.length = sizeof(struct virtio_net_header) + NET_PACKET_SIZE,
		.direction = DMA_FROM_DEVICE,
	};

	return virtqueue_add(network->rx_queue, &buffer, 1, receive);
}

static void virtio_net_free_rx(struct virtio_net_rx_buffer *receive)
{
	if (!receive)
		return;
	if (receive->page)
		pfree(receive->page);
	free(receive);
}

static void virtio_net_reap_tx(struct virtio_net *network)
{
	struct virtio_net_tx_request *transmit;
	int completed = 0;

	while ((transmit = virtqueue_get_used(network->tx_queue, 0))) {
		net_packet_put(transmit->packet);
		free(transmit);
		completed = 1;
	}
	if (completed)
		netif_wake_queue(&network->netdev);
}

static void virtio_net_poll(struct work_struct *work)
{
	struct virtio_net *network =
		container_of(work, struct virtio_net, poll_work);
	struct virtio_net_rx_buffer *receive;
	struct net_packet *packet;
	uint32 budget = VIRTIO_NET_RX_BUDGET;
	uint32 length;
	int kick = 0;

	virtio_net_reap_tx(network);
	while (budget-- && (receive = virtqueue_get_used(
		       network->rx_queue, &length))) {
		if (__atomic_load_n(&network->rx_enabled,
				    __ATOMIC_ACQUIRE) &&
		    length > sizeof(struct virtio_net_header) &&
		    length <= sizeof(struct virtio_net_header) +
			      NET_ETH_HEADER_LENGTH + NET_ETH_MTU) {
			struct virtio_net_header *header = receive->page;

			length -= sizeof(struct virtio_net_header);
			packet = header->flags || header->gso_type ||
				length < NET_ETH_HEADER_LENGTH ? 0 :
				net_packet_alloc(length);
			if (packet) {
				memmove(packet->data,
					receive->page +
						sizeof(struct virtio_net_header),
					length);
				packet->length = length;
				packet->device = &network->netdev;
				netif_receive(packet);
			} else {
				net_device_rx_drop(&network->netdev);
			}
		} else {
			net_device_rx_drop(&network->netdev);
		}
		if (virtio_net_post_rx(network, receive) < 0) {
			virtio_net_free_rx(receive);
			network->rx_buffers--;
		} else {
			kick = 1;
		}
	}
	if (kick)
		virtqueue_kick(network->rx_queue);
	if (virtqueue_has_used(network->rx_queue) ||
	    virtqueue_has_used(network->tx_queue))
		schedule_work(&network->poll_work);
	virtio_net_update_carrier(network);
}

static void virtio_net_queue_done(struct virtqueue *queue)
{
	struct virtio_net *network = queue->private;

	if (network)
		schedule_work(&network->poll_work);
}

static int virtio_net_open(struct net_device *device)
{
	struct virtio_net *network = device->private;

	__atomic_store_n(&network->rx_enabled, 1, __ATOMIC_RELEASE);
	virtqueue_kick(network->rx_queue);
	schedule_work(&network->poll_work);
	return 0;
}

static void virtio_net_stop(struct net_device *device)
{
	struct virtio_net *network = device->private;

	__atomic_store_n(&network->rx_enabled, 0, __ATOMIC_RELEASE);
	cancel_work_sync(&network->poll_work);
	virtio_net_reap_tx(network);
}

static int virtio_net_xmit(struct net_device *device,
			   struct net_packet *packet)
{
	struct virtio_net *network = device->private;
	struct virtio_net_tx_request *request;
	struct virtio_buffer buffers[2];
	int status;

	request = calloc(1, sizeof(*request));
	if (!request)
		return -1;
	request->packet = packet;
	buffers[0].address = &request->header;
	buffers[0].length = sizeof(request->header);
	buffers[0].direction = DMA_TO_DEVICE;
	buffers[1].address = packet->data;
	buffers[1].length = packet->length;
	buffers[1].direction = DMA_TO_DEVICE;
	status = virtqueue_add(network->tx_queue, buffers, 2, request);
	if (status < 0) {
		free(request);
		if (status != -1)
			return -1;
		netif_stop_queue(device);
		if (virtqueue_num_free(network->tx_queue) >= 2)
			netif_wake_queue(device);
		return NETDEV_TX_BUSY;
	}
	if (virtqueue_num_free(network->tx_queue) < 2) {
		netif_stop_queue(device);
		if (virtqueue_num_free(network->tx_queue) >= 2)
			netif_wake_queue(device);
	}
	virtqueue_kick(network->tx_queue);
	return 0;
}

static const struct net_device_operations virtio_net_operations = {
	.open = virtio_net_open,
	.stop = virtio_net_stop,
	.start_xmit = virtio_net_xmit,
};

static int virtio_net_fill_rx(struct virtio_net *network)
{
	uint32 count, index;

	count = virtqueue_num_free(network->rx_queue);
	if (count > VIRTIO_NET_RX_BUFFERS)
		count = VIRTIO_NET_RX_BUFFERS;
	for (index = 0; index < count; index++) {
		struct virtio_net_rx_buffer *receive;

		receive = calloc(1, sizeof(*receive));
		if (!receive)
			return -1;
		receive->page = palloc_zero();
		if (!receive->page) {
			free(receive);
			return -1;
		}
		if (virtio_net_post_rx(network, receive) < 0) {
			virtio_net_free_rx(receive);
			return -1;
		}
		network->rx_buffers++;
	}
	return 0;
}

static int virtio_net_probe(struct virtio_device *device)
{
	static const char *const names[] = {
		"virtio-net receive", "virtio-net transmit",
	};
	void (*callbacks[])(struct virtqueue *) = {
		virtio_net_queue_done, virtio_net_queue_done,
	};
	struct virtqueue *queues[VIRTIO_NET_QUEUE_COUNT];
	struct virtio_net *network;

	network = calloc(1, sizeof(*network));
	if (!network)
		return DRIVER_ERR_BUSY;
	network->virtio = device;
	work_init(&network->poll_work, virtio_net_poll);
	if (virtio_find_vqs(device, VIRTIO_NET_QUEUE_COUNT, queues,
			    callbacks, names) < 0)
		goto failed;
	network->rx_queue = queues[VIRTIO_NET_RX_QUEUE];
	network->tx_queue = queues[VIRTIO_NET_TX_QUEUE];
	if (virtqueue_num_free(network->tx_queue) < 2)
		goto failed;
	network->rx_queue->private = network;
	network->tx_queue->private = network;
	if (virtio_net_fill_rx(network) < 0)
		goto failed;
	network->netdev.parent = &device->device;
	network->netdev.mtu = NET_ETH_MTU;
	network->netdev.operations = &virtio_net_operations;
	network->netdev.private = network;
	if (virtio_has_feature(device, VIRTIO_NET_F_MAC))
		device->config->get_config(device, 0,
			network->netdev.address,
			sizeof(network->netdev.address));
	else {
		network->netdev.address[0] = 0x02;
		network->netdev.address[5] = device->transport_index;
	}
	if (net_device_register(&network->netdev) < 0)
		goto failed;
	dev_set_drvdata(&device->device, network);
	pr_info("%s: virtio-net MAC %02x:%02x:%02x:%02x:%02x:%02x",
		network->netdev.name, network->netdev.address[0],
		network->netdev.address[1], network->netdev.address[2],
		network->netdev.address[3], network->netdev.address[4],
		network->netdev.address[5]);
	return DRIVER_OK;

failed:
	if (network->rx_queue) {
		struct virtio_net_rx_buffer *receive;

		while ((receive = virtqueue_detach_unused(
			       network->rx_queue)))
			virtio_net_free_rx(receive);
		device->config->del_vqs(device);
	}
	free(network);
	return DRIVER_ERR_NODEV;
}

static void virtio_net_ready(struct virtio_device *device)
{
	struct virtio_net *network = dev_get_drvdata(&device->device);

	if (!network)
		return;
	virtio_net_update_carrier(network);
	if (net_device_open(&network->netdev) < 0)
		PANIC("open virtio-net device");
}

static void virtio_net_config_changed(struct virtio_device *device)
{
	struct virtio_net *network = dev_get_drvdata(&device->device);

	if (network)
		schedule_work(&network->poll_work);
}

static void virtio_net_remove(struct virtio_device *device)
{
	struct virtio_net *network = dev_get_drvdata(&device->device);
	struct virtio_net_rx_buffer *receive;
	struct virtio_net_tx_request *transmit;

	if (!network)
		return;
	cancel_work_sync(&network->poll_work);
	if (net_device_unregister(&network->netdev) < 0)
		PANIC("unregister virtio-net device");
	while ((receive = virtqueue_detach_unused(network->rx_queue)))
		virtio_net_free_rx(receive);
	while ((transmit = virtqueue_detach_unused(network->tx_queue))) {
		net_packet_put(transmit->packet);
		free(transmit);
	}
	dev_set_drvdata(&device->device, 0);
	free(network);
}

static const struct virtio_device_id virtio_net_ids[] = {
	{ VIRTIO_ID_NET, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_net_driver = {
	.driver = {
		.name = "virtio-net",
	},
	.id_table = virtio_net_ids,
	.feature_table = (1ULL << VIRTIO_NET_F_MAC) |
			 (1ULL << VIRTIO_NET_F_STATUS),
	.probe = virtio_net_probe,
	.ready = virtio_net_ready,
	.config_changed = virtio_net_config_changed,
	.remove = virtio_net_remove,
};

int virtio_net_init(void)
{
	return virtio_driver_register(&virtio_net_driver);
}
