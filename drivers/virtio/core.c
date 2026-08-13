#include <virtio.h>

static int virtio_match(struct device *device,
			struct device_driver *driver)
{
	struct virtio_device *virtio_device = to_virtio_device(device);
	struct virtio_driver *virtio_driver = to_virtio_driver(driver);
	const struct virtio_device_id *id;

	for (id = virtio_driver->id_table; id && id->device; id++) {
		if (id->device != virtio_device->id)
			continue;
		if (id->vendor == VIRTIO_DEV_ANY_ID ||
		    id->vendor == virtio_device->vendor)
			return 1;
	}
	return 0;
}

static int virtio_probe(struct device *device)
{
	struct virtio_device *virtio_device = to_virtio_device(device);
	struct virtio_driver *driver = to_virtio_driver(device->driver);
	uint64 offered;
	uint8 status;
	int result;

	virtio_device->config->reset(virtio_device);
	status = VIRTIO_CONFIG_S_ACKNOWLEDGE | VIRTIO_CONFIG_S_DRIVER;
	virtio_device->config->set_status(virtio_device, status);
	offered = virtio_device->config->get_features(virtio_device);
	if (!(offered & (1ULL << VIRTIO_F_VERSION_1))) {
		result = DRIVER_ERR_NODEV;
		goto failed;
	}
	virtio_device->features = offered & driver->feature_table;
	virtio_device->features |= 1ULL << VIRTIO_F_VERSION_1;
	result = virtio_device->config->finalize_features(virtio_device);
	if (result < 0)
		goto failed;
	status |= VIRTIO_CONFIG_S_FEATURES_OK;
	virtio_device->config->set_status(virtio_device, status);
	status = virtio_device->config->get_status(virtio_device);
	if (!(status & VIRTIO_CONFIG_S_FEATURES_OK)) {
		result = DRIVER_ERR_NODEV;
		goto failed;
	}
	result = driver->probe ? driver->probe(virtio_device) : DRIVER_OK;
	if (result < 0)
		goto failed;
	status = virtio_device->config->get_status(virtio_device);
	virtio_device->config->set_status(
		virtio_device, status | VIRTIO_CONFIG_S_DRIVER_OK);
	if (driver->ready)
		driver->ready(virtio_device);
	return DRIVER_OK;

failed:
	status = virtio_device->config->get_status(virtio_device);
	virtio_device->config->set_status(
		virtio_device, status | VIRTIO_CONFIG_S_FAILED);
	virtio_device->config->reset(virtio_device);
	return result;
}

static void virtio_remove(struct device *device)
{
	struct virtio_device *virtio_device = to_virtio_device(device);
	struct virtio_driver *driver = to_virtio_driver(device->driver);

	virtio_device->config->reset(virtio_device);
	if (driver->remove)
		driver->remove(virtio_device);
	virtio_device->config->del_vqs(virtio_device);
}

static struct bus_type virtio_bus = {
	.name = "virtio",
	.match = virtio_match,
};

void virtio_bus_init(void)
{
	bus_register(&virtio_bus);
}

int virtio_device_register(struct virtio_device *device)
{
	if (!device || !device->config || !device->id)
		return DRIVER_ERR_INVAL;
	device->device.bus = &virtio_bus;
	return device_register(&device->device);
}

void virtio_device_unregister(struct virtio_device *device)
{
	if (device)
		device_unregister(&device->device);
}

int virtio_driver_register(struct virtio_driver *driver)
{
	if (!driver || !driver->id_table)
		return DRIVER_ERR_INVAL;
	driver->driver.bus = &virtio_bus;
	driver->driver.probe = virtio_probe;
	driver->driver.remove = virtio_remove;
	return driver_register(&driver->driver);
}

void virtio_driver_unregister(struct virtio_driver *driver)
{
	if (driver)
		driver_unregister(&driver->driver);
}

int virtio_has_feature(struct virtio_device *device, uint32 feature)
{
	return device && feature < 64 &&
	       !!(device->features & (1ULL << feature));
}

void virtio_config_changed(struct virtio_device *device)
{
	struct virtio_driver *driver;

	if (!device || !device->device.driver)
		return;
	driver = to_virtio_driver(device->device.driver);
	if (driver->config_changed)
		driver->config_changed(device);
}

int virtio_find_vqs(struct virtio_device *device, uint16 count,
		    struct virtqueue **queues,
		    void (**callbacks)(struct virtqueue *queue),
		    const char *const names[])
{
	if (!device || !device->config || !device->config->find_vqs)
		return DRIVER_ERR_INVAL;
	return device->config->find_vqs(device, count, queues, callbacks,
				       names);
}
