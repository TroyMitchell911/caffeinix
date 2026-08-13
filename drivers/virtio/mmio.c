#include <dma.h>
#include <io.h>
#include <irq.h>
#include <mystring.h>
#include <of.h>
#include <palloc.h>
#include <platform_device.h>
#include <resource.h>
#include <virtio.h>
#include <virtio_ring.h>

#define VIRTIO_MMIO_MAGIC_VALUE 0x000
#define VIRTIO_MMIO_VERSION 0x004
#define VIRTIO_MMIO_DEVICE_ID 0x008
#define VIRTIO_MMIO_VENDOR_ID 0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES 0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES 0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL 0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX 0x034
#define VIRTIO_MMIO_QUEUE_NUM 0x038
#define VIRTIO_MMIO_QUEUE_READY 0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY 0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK 0x064
#define VIRTIO_MMIO_STATUS 0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW 0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH 0x084
#define VIRTIO_MMIO_DRIVER_DESC_LOW 0x090
#define VIRTIO_MMIO_DRIVER_DESC_HIGH 0x094
#define VIRTIO_MMIO_DEVICE_DESC_LOW 0x0a0
#define VIRTIO_MMIO_DEVICE_DESC_HIGH 0x0a4
#define VIRTIO_MMIO_CONFIG 0x100

#define VIRTIO_MMIO_MAGIC 0x74726976
#define VIRTIO_MMIO_MODERN 2
#define VIRTIO_MMIO_MAX_QUEUES 4
#define VIRTIO_MMIO_QUEUE_SIZE 128

struct virtio_mmio_device {
	struct virtio_device virtio;
	struct platform_device *platform;
	volatile uint8 *base;
	uint64 size;
	uint32 irq;
	struct virtqueue *queues[VIRTIO_MMIO_MAX_QUEUES];
	uint16 queue_count;
	char name[32];
};

static uint32 mmio_read(struct virtio_mmio_device *device, uint32 offset)
{
	return readl(device->base + offset);
}

static void mmio_write(struct virtio_mmio_device *device, uint32 offset,
		       uint32 value)
{
	writel(value, (void *)(device->base + offset));
}

static uint64 virtio_mmio_get_features(struct virtio_device *virtio)
{
	struct virtio_mmio_device *device = virtio->private;
	uint64 features;

	mmio_write(device, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 0);
	features = mmio_read(device, VIRTIO_MMIO_DEVICE_FEATURES);
	mmio_write(device, VIRTIO_MMIO_DEVICE_FEATURES_SEL, 1);
	features |= (uint64)mmio_read(device,
				     VIRTIO_MMIO_DEVICE_FEATURES) << 32;
	return features;
}

static int virtio_mmio_finalize_features(struct virtio_device *virtio)
{
	struct virtio_mmio_device *device = virtio->private;

	if (!(virtio->features & (1ULL << VIRTIO_F_VERSION_1)))
		return DRIVER_ERR_NODEV;
	mmio_write(device, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 0);
	mmio_write(device, VIRTIO_MMIO_DRIVER_FEATURES,
		   (uint32)virtio->features);
	mmio_write(device, VIRTIO_MMIO_DRIVER_FEATURES_SEL, 1);
	mmio_write(device, VIRTIO_MMIO_DRIVER_FEATURES,
		   (uint32)(virtio->features >> 32));
	return DRIVER_OK;
}

static void virtio_mmio_notify(struct virtqueue *queue)
{
	struct virtio_mmio_device *device = queue->device->private;

	mmio_write(device, VIRTIO_MMIO_QUEUE_NOTIFY, queue->index);
}

static int virtio_mmio_find_vqs(struct virtio_device *virtio,
				uint16 count, struct virtqueue **queues,
				void (**callbacks)(struct virtqueue *queue),
				const char *const names[])
{
	struct virtio_mmio_device *device = virtio->private;
	uint32 maximum;
	uint16 index, size;

	if (!count || count > VIRTIO_MMIO_MAX_QUEUES ||
	    device->queue_count)
		return DRIVER_ERR_INVAL;
	for (index = 0; index < count; index++) {
		mmio_write(device, VIRTIO_MMIO_QUEUE_SEL, index);
		if (mmio_read(device, VIRTIO_MMIO_QUEUE_READY))
			goto failed;
		maximum = mmio_read(device, VIRTIO_MMIO_QUEUE_NUM_MAX);
		if (!maximum)
			goto failed;
		size = maximum < VIRTIO_MMIO_QUEUE_SIZE ? maximum :
			VIRTIO_MMIO_QUEUE_SIZE;
		while (size & (size - 1))
			size &= size - 1;
		queues[index] = virtqueue_create(
			virtio, index, size, callbacks ? callbacks[index] : 0,
			virtio_mmio_notify, names ? names[index] : "virtqueue");
		if (!queues[index])
			goto failed;
		device->queues[index] = queues[index];
		mmio_write(device, VIRTIO_MMIO_QUEUE_NUM, size);
		mmio_write(device, VIRTIO_MMIO_QUEUE_DESC_LOW,
			   (uint32)queues[index]->descriptors_dma);
		mmio_write(device, VIRTIO_MMIO_QUEUE_DESC_HIGH,
			   (uint32)(queues[index]->descriptors_dma >> 32));
		mmio_write(device, VIRTIO_MMIO_DRIVER_DESC_LOW,
			   (uint32)queues[index]->available_dma);
		mmio_write(device, VIRTIO_MMIO_DRIVER_DESC_HIGH,
			   (uint32)(queues[index]->available_dma >> 32));
		mmio_write(device, VIRTIO_MMIO_DEVICE_DESC_LOW,
			   (uint32)queues[index]->used_dma);
		mmio_write(device, VIRTIO_MMIO_DEVICE_DESC_HIGH,
			   (uint32)(queues[index]->used_dma >> 32));
		mmio_write(device, VIRTIO_MMIO_QUEUE_READY, 1);
		device->queue_count++;
	}
	return DRIVER_OK;

failed:
	while (device->queue_count) {
		index = --device->queue_count;
		mmio_write(device, VIRTIO_MMIO_QUEUE_SEL, index);
		mmio_write(device, VIRTIO_MMIO_QUEUE_READY, 0);
		virtqueue_destroy(device->queues[index]);
		device->queues[index] = 0;
	}
	return DRIVER_ERR_NODEV;
}

static void virtio_mmio_del_vqs(struct virtio_device *virtio)
{
	struct virtio_mmio_device *device = virtio->private;
	uint16 index;

	while (device->queue_count) {
		index = --device->queue_count;
		mmio_write(device, VIRTIO_MMIO_QUEUE_SEL, index);
		mmio_write(device, VIRTIO_MMIO_QUEUE_READY, 0);
		virtqueue_destroy(device->queues[index]);
		device->queues[index] = 0;
	}
}

static void virtio_mmio_reset(struct virtio_device *virtio)
{
	struct virtio_mmio_device *device = virtio->private;

	mmio_write(device, VIRTIO_MMIO_STATUS, 0);
	dma_mb();
}

static uint8 virtio_mmio_get_status(struct virtio_device *virtio)
{
	struct virtio_mmio_device *device = virtio->private;

	return mmio_read(device, VIRTIO_MMIO_STATUS);
}

static void virtio_mmio_set_status(struct virtio_device *virtio,
				   uint8 status)
{
	struct virtio_mmio_device *device = virtio->private;

	mmio_write(device, VIRTIO_MMIO_STATUS, status);
}

static void virtio_mmio_get_config(struct virtio_device *virtio,
				   uint32 offset, void *buffer,
				   uint32 length)
{
	struct virtio_mmio_device *device = virtio->private;
	uint8 *bytes = buffer;
	uint32 index;

	for (index = 0; index < length; index++)
		bytes[index] = readb(device->base + VIRTIO_MMIO_CONFIG +
				     offset + index);
}

static void virtio_mmio_set_config(struct virtio_device *virtio,
				   uint32 offset, const void *buffer,
				   uint32 length)
{
	struct virtio_mmio_device *device = virtio->private;
	const uint8 *bytes = buffer;
	uint32 index;

	for (index = 0; index < length; index++)
		writeb(bytes[index], (void *)(device->base +
		       VIRTIO_MMIO_CONFIG + offset + index));
}

static const struct virtio_config_ops virtio_mmio_config_ops = {
	.get_features = virtio_mmio_get_features,
	.finalize_features = virtio_mmio_finalize_features,
	.find_vqs = virtio_mmio_find_vqs,
	.del_vqs = virtio_mmio_del_vqs,
	.reset = virtio_mmio_reset,
	.get_status = virtio_mmio_get_status,
	.set_status = virtio_mmio_set_status,
	.get_config = virtio_mmio_get_config,
	.set_config = virtio_mmio_set_config,
};

static int virtio_mmio_irq(uint32 irq, void *data)
{
	struct virtio_mmio_device *device = data;
	uint32 status;
	uint16 index;

	if (irq != device->irq)
		return IRQ_NONE;
	status = mmio_read(device, VIRTIO_MMIO_INTERRUPT_STATUS) & 3;
	if (!status)
		/* The PLIC may retain a request after the level is cleared. */
		return IRQ_HANDLED;
	mmio_write(device, VIRTIO_MMIO_INTERRUPT_ACK, status);
	dma_mb();
	if (status & 1) {
		for (index = 0; index < device->queue_count; index++) {
			struct virtqueue *queue = device->queues[index];

			if (queue && queue->callback &&
			    virtqueue_has_used(queue))
				queue->callback(queue);
		}
	}
	if (status & 2)
		virtio_config_changed(&device->virtio);
	return IRQ_HANDLED;
}

static uint32 virtio_mmio_transport_index(uint64 address)
{
	struct device_node *node = 0;
	struct resource resource;
	uint32 index = 0;

	while ((node = of_next_node(node))) {
		if (!of_device_is_available(node) ||
		    !of_device_is_compatible(node, "virtio,mmio") ||
		    of_address_to_resource(node, 0, &resource) < 0)
			continue;
		if (resource.start < address)
			index++;
	}
	return index;
}

static void virtio_mmio_release(struct device *device)
{
	struct virtio_mmio_device *mmio;

	mmio = container_of(to_virtio_device(device),
			    struct virtio_mmio_device, virtio);
	free(mmio);
}

static int virtio_mmio_probe(struct platform_device *platform)
{
	struct virtio_mmio_device *device;
	struct resource *resource;
	uint32 id;
	int status;

	resource = platform_get_resource(platform, RESOURCE_MEM, 0);
	if (!resource)
		return DRIVER_ERR_NODEV;
	device = calloc(1, sizeof(*device));
	if (!device)
		return DRIVER_ERR_BUSY;
	device->base = ioremap(resource->start, resource_size(resource));
	device->size = resource_size(resource);
	device->irq = platform_get_irq(platform, 0);
	if (!device->base || device->irq <= 0 ||
	    mmio_read(device, VIRTIO_MMIO_MAGIC_VALUE) !=
		VIRTIO_MMIO_MAGIC ||
	    mmio_read(device, VIRTIO_MMIO_VERSION) != VIRTIO_MMIO_MODERN)
		goto invalid;
	id = mmio_read(device, VIRTIO_MMIO_DEVICE_ID);
	if (!id) {
		free(device);
		return DRIVER_OK;
	}
	device->platform = platform;
	device->virtio.id = id;
	device->virtio.vendor = mmio_read(device, VIRTIO_MMIO_VENDOR_ID);
	device->virtio.transport_index =
		virtio_mmio_transport_index(resource->start);
	device->virtio.config = &virtio_mmio_config_ops;
	device->virtio.private = device;
	device->virtio.device.parent = &platform->device;
	device->virtio.device.release = virtio_mmio_release;
	device->virtio.device.dma_mask = ~(uint64)0;
	safe_strncpy(device->name, "virtio-mmio0", sizeof(device->name));
	device->name[11] = '0' + device->virtio.transport_index;
	device->virtio.device.name = device->name;
	status = request_irq(device->irq, virtio_mmio_irq, device,
			     device->name);
	if (status < 0)
		goto invalid;
	dev_set_drvdata(&platform->device, device);
	status = virtio_device_register(&device->virtio);
	if (status < 0) {
		dev_set_drvdata(&platform->device, 0);
		free_irq(device->irq, device);
		goto invalid;
	}
	return DRIVER_OK;

invalid:
	free(device);
	return DRIVER_ERR_NODEV;
}

static void virtio_mmio_remove(struct platform_device *platform)
{
	struct virtio_mmio_device *device =
		dev_get_drvdata(&platform->device);

	if (!device)
		return;
	free_irq(device->irq, device);
	dev_set_drvdata(&platform->device, 0);
	virtio_device_unregister(&device->virtio);
}

static const struct of_device_id virtio_mmio_matches[] = {
	{ .compatible = "virtio,mmio" },
	{ 0 },
};

static struct platform_driver virtio_mmio_driver = {
	.driver = {
		.name = "virtio-mmio",
	},
	.of_match_table = virtio_mmio_matches,
	.probe = virtio_mmio_probe,
	.remove = virtio_mmio_remove,
};

int virtio_mmio_init(void)
{
	return platform_driver_register(&virtio_mmio_driver);
}
