#include <mystring.h>
#include <of.h>
#include <platform_device.h>

#define PLATFORM_DEVICE_MAX 32

struct platform_slot {
	struct platform_device device;
	char name[OF_PATH_MAX];
	uint8 used;
};

static struct platform_slot platform_devices[PLATFORM_DEVICE_MAX];

static const struct of_device_id *platform_match_id(
				const struct of_device_id *matches,
				struct device_node *node)
{
	if (!matches || !node)
		return 0;
	for (; matches->compatible; matches++) {
		if (of_device_is_compatible(node, matches->compatible))
			return matches;
	}
	return 0;
}

static int platform_match(struct device *device,
			  struct device_driver *driver)
{
	struct platform_device *platform_device;
	struct platform_driver *platform_driver;
	const struct of_device_id *match;

	platform_device = to_platform_device(device);
	platform_driver = to_platform_driver(driver);
	if (platform_device->compatible) {
		for (match = platform_driver->of_match_table;
		     match && match->compatible; match++) {
			if (!strcmp(match->compatible,
			            platform_device->compatible))
				return 1;
		}
		return 0;
	}
	return platform_match_id(platform_driver->of_match_table,
	                         device->of_node) != 0;
}

static int platform_probe(struct device *device)
{
	struct platform_driver *driver;

	driver = to_platform_driver(device->driver);
	return driver->probe ? driver->probe(to_platform_device(device)) : 0;
}

static void platform_remove(struct device *device)
{
	struct platform_driver *driver;

	driver = to_platform_driver(device->driver);
	if (driver->remove)
		driver->remove(to_platform_device(device));
}

static struct bus_type platform_bus = {
	.name = "platform",
	.match = platform_match,
};

void platform_bus_init(void)
{
	bus_register(&platform_bus);
}

int platform_device_register(struct platform_device *device)
{
	if (!device)
		return DRIVER_ERR_INVAL;
	device->device.bus = &platform_bus;
	return device_register(&device->device);
}

void platform_device_unregister(struct platform_device *device)
{
	if (device)
		device_unregister(&device->device);
}

int platform_driver_register(struct platform_driver *driver)
{
	if (!driver || !driver->of_match_table)
		return DRIVER_ERR_INVAL;
	driver->driver.bus = &platform_bus;
	driver->driver.probe = platform_probe;
	driver->driver.remove = platform_remove;
	return driver_register(&driver->driver);
}

void platform_driver_unregister(struct platform_driver *driver)
{
	if (driver)
		driver_unregister(&driver->driver);
}

static void platform_device_release(struct device *device)
{
	struct platform_slot *slot;

	slot = container_of(to_platform_device(device),
	                    struct platform_slot, device);
	memset(slot, 0, sizeof(*slot));
}

static struct platform_slot *platform_slot_alloc(void)
{
	int index;

	for (index = 0; index < PLATFORM_DEVICE_MAX; index++) {
		if (!platform_devices[index].used) {
			platform_devices[index].used = 1;
			return &platform_devices[index];
		}
	}
	return 0;
}

static int platform_device_from_node(struct device_node *node)
{
	struct platform_device *device;
	struct platform_slot *slot;
	struct resource resource;
	int index, irq, status;

	if (!of_get_property(node, "compatible", 0) ||
	    !of_get_property(node, "reg", 0) ||
	    !of_device_is_available(node))
		return 0;
	slot = platform_slot_alloc();
	if (!slot)
		return DRIVER_ERR_BUSY;
	device = &slot->device;
	if (of_node_path(node, slot->name, sizeof(slot->name)) < 0) {
		platform_device_release(&device->device);
		return DRIVER_ERR_INVAL;
	}
	device->device.name = slot->name;
	device->device.of_node = node;
	device->device.release = platform_device_release;
	device->id = -1;
	for (index = 0; index < PLATFORM_RESOURCE_MAX; index++) {
		if (of_address_to_resource(node, index, &resource) < 0)
			break;
		device->resources[device->resource_count++] = resource;
	}
	if (!device->resource_count) {
		platform_device_release(&device->device);
		return 0;
	}
	for (index = 0; device->resource_count < PLATFORM_RESOURCE_MAX;
	     index++) {
		irq = of_irq_get(node, index);
		if (irq < 0)
			break;
		resource.name = node->name;
		resource.start = irq;
		resource.end = irq;
		resource.flags = RESOURCE_IRQ;
		device->resources[device->resource_count++] = resource;
	}
	status = platform_device_register(device);
	if (status < 0)
		platform_device_release(&device->device);
	return status;
}

int of_platform_populate(void)
{
	struct device_node *node = 0;
	int status;

	while ((node = of_next_node(node))) {
		status = platform_device_from_node(node);
		if (status < 0)
			return status;
	}
	return 0;
}

struct resource *platform_get_resource(struct platform_device *device,
				       uint32 type, uint32 index)
{
	uint32 found = 0, current;

	if (!device)
		return 0;
	for (current = 0; current < device->resource_count; current++) {
		if (!(device->resources[current].flags & type))
			continue;
		if (found++ == index)
			return &device->resources[current];
	}
	return 0;
}

int platform_get_irq(struct platform_device *device, uint32 index)
{
	struct resource *resource;

	resource = platform_get_resource(device, RESOURCE_IRQ, index);
	return resource ? resource->start : DRIVER_ERR_NODEV;
}

const struct of_device_id *platform_get_match_data(
				struct platform_device *device)
{
	const struct of_device_id *match;
	struct platform_driver *driver;

	if (!device || !device->device.driver)
		return 0;
	driver = to_platform_driver(device->device.driver);
	if (device->compatible) {
		for (match = driver->of_match_table;
		     match && match->compatible; match++) {
			if (!strcmp(match->compatible, device->compatible))
				return match;
		}
		return 0;
	}
	return platform_match_id(driver->of_match_table,
	                         device->device.of_node);
}
