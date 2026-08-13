#include <device_model.h>
#include <mystring.h>

static struct {
	struct spinlock lock;
	struct list buses;
	uint8 initialized;
} driver_core;

void driver_core_init(void)
{
	spinlock_init(&driver_core.lock, "driver core");
	list_init(&driver_core.buses);
	driver_core.initialized = 1;
}

static int bus_has_name(struct bus_type *bus, const char *name)
{
	struct list *node;

	for (node = bus->devices.next; node != &bus->devices;
	     node = node->next) {
		struct device *device = list_entry(node, struct device, node);

		if (!strcmp(device->name, name))
			return 1;
	}
	return 0;
}

int bus_register(struct bus_type *bus)
{
	struct list *node;

	if (!driver_core.initialized || !bus || !bus->name || !bus->match)
		return DRIVER_ERR_INVAL;
	spinlock_acquire(&driver_core.lock);
	if (bus->registered) {
		spinlock_release(&driver_core.lock);
		return DRIVER_ERR_EXIST;
	}
	for (node = driver_core.buses.next; node != &driver_core.buses;
	     node = node->next) {
		struct bus_type *existing;

		existing = list_entry(node, struct bus_type, node);
		if (!strcmp(existing->name, bus->name)) {
			spinlock_release(&driver_core.lock);
			return DRIVER_ERR_EXIST;
		}
	}
	spinlock_init(&bus->lock, bus->name);
	list_init(&bus->devices);
	list_init(&bus->drivers);
	list_init(&bus->node);
	bus->registered = 1;
	list_insert_before(&driver_core.buses, &bus->node);
	spinlock_release(&driver_core.lock);
	return DRIVER_OK;
}

int bus_unregister(struct bus_type *bus)
{
	if (!driver_core.initialized || !bus)
		return DRIVER_ERR_INVAL;
	spinlock_acquire(&driver_core.lock);
	if (!bus->registered) {
		spinlock_release(&driver_core.lock);
		return DRIVER_ERR_INVAL;
	}
	spinlock_acquire(&bus->lock);
	if (bus->devices.next != &bus->devices ||
	    bus->drivers.next != &bus->drivers) {
		spinlock_release(&bus->lock);
		spinlock_release(&driver_core.lock);
		return DRIVER_ERR_BUSY;
	}
	list_remove(&bus->node);
	bus->registered = 0;
	spinlock_release(&bus->lock);
	spinlock_release(&driver_core.lock);
	return DRIVER_OK;
}

static int device_try_driver(struct device *device,
			     struct device_driver *driver)
{
	int status;

	spinlock_acquire(&device->bus->lock);
	if (!device->registered || device->state != DEVICE_UNBOUND ||
	    !driver->registered || !device->bus->match(device, driver)) {
		spinlock_release(&device->bus->lock);
		return DRIVER_ERR_NODEV;
	}
	device->state = DEVICE_PROBING;
	device->driver = driver;
	spinlock_release(&device->bus->lock);

	status = driver->probe ? driver->probe(device) : DRIVER_OK;

	spinlock_acquire(&device->bus->lock);
	if (status == DRIVER_OK) {
		device->state = DEVICE_BOUND;
	} else {
		device->driver = 0;
		device->driver_data = 0;
		device->state = DEVICE_UNBOUND;
	}
	spinlock_release(&device->bus->lock);
	return status;
}

int device_register(struct device *device)
{
	struct list *node;
	struct device *parent = 0;
	int status;

	if (!device || !device->name || !device->bus ||
	    !device->bus->registered || !device->release)
		return DRIVER_ERR_INVAL;
	if (device->parent && !device->parent->registered)
		return DRIVER_ERR_INVAL;
	if (device->parent) {
		parent = device_get(device->parent);
		if (!parent)
			return DRIVER_ERR_INVAL;
	}
	spinlock_acquire(&device->bus->lock);
	if (device->registered || bus_has_name(device->bus, device->name)) {
		spinlock_release(&device->bus->lock);
		device_put(parent);
		return DRIVER_ERR_EXIST;
	}
	list_init(&device->node);
	list_init(&device->children);
	list_init(&device->sibling);
	device->driver = 0;
	device->driver_data = 0;
	if (!device->dma_mask)
		device->dma_mask = ~(uint64)0;
	device->state = DEVICE_UNBOUND;
	device->refcount = 1;
	device->registered = 1;
	list_insert_before(&device->bus->devices, &device->node);
	spinlock_release(&device->bus->lock);
	if (parent) {
		spinlock_acquire(&driver_core.lock);
		list_insert_before(&parent->children, &device->sibling);
		spinlock_release(&driver_core.lock);
	}

	for (node = device->bus->drivers.next;
	     node != &device->bus->drivers; node = node->next) {
		struct device_driver *driver;

		driver = list_entry(node, struct device_driver, node);
		status = device_try_driver(device, driver);
		if (status == DRIVER_OK)
			break;
	}
	return DRIVER_OK;
}

struct device *device_get(struct device *device)
{
	if (!device || !device->registered)
		return 0;
	spinlock_acquire(&device->bus->lock);
	if (!device->refcount) {
		spinlock_release(&device->bus->lock);
		return 0;
	}
	device->refcount++;
	spinlock_release(&device->bus->lock);
	return device;
}

void device_put(struct device *device)
{
	void (*release)(struct device *device) = 0;

	if (!device)
		return;
	spinlock_acquire(&device->bus->lock);
	if (device->refcount && !--device->refcount)
		release = device->release;
	spinlock_release(&device->bus->lock);
	if (release)
		release(device);
}

void device_unregister(struct device *device)
{
	struct device *parent;
	struct device_driver *driver;

	if (!device || !device->bus || !device->registered)
		return;
	spinlock_acquire(&device->bus->lock);
	driver = device->state == DEVICE_BOUND ? device->driver : 0;
	device->state = DEVICE_PROBING;
	device->registered = 0;
	list_remove(&device->node);
	parent = device->parent;
	device->parent = 0;
	spinlock_release(&device->bus->lock);
	if (parent) {
		spinlock_acquire(&driver_core.lock);
		list_remove(&device->sibling);
		spinlock_release(&driver_core.lock);
	}
	if (driver && driver->remove)
		driver->remove(device);
	spinlock_acquire(&device->bus->lock);
	device->state = DEVICE_UNBOUND;
	device->driver = 0;
	device->driver_data = 0;
	spinlock_release(&device->bus->lock);
	device_put(device);
	device_put(parent);
}

int driver_register(struct device_driver *driver)
{
	struct list *node;

	if (!driver || !driver->name || !driver->bus ||
	    !driver->bus->registered)
		return DRIVER_ERR_INVAL;
	spinlock_acquire(&driver->bus->lock);
	if (driver->registered) {
		spinlock_release(&driver->bus->lock);
		return DRIVER_ERR_EXIST;
	}
	for (node = driver->bus->drivers.next;
	     node != &driver->bus->drivers; node = node->next) {
		struct device_driver *existing;

		existing = list_entry(node, struct device_driver, node);
		if (!strcmp(existing->name, driver->name)) {
			spinlock_release(&driver->bus->lock);
			return DRIVER_ERR_EXIST;
		}
	}
	list_init(&driver->node);
	driver->registered = 1;
	list_insert_before(&driver->bus->drivers, &driver->node);
	spinlock_release(&driver->bus->lock);

	for (node = driver->bus->devices.next;
	     node != &driver->bus->devices; node = node->next) {
		struct device *device = list_entry(node, struct device, node);

		device_try_driver(device, driver);
	}
	return DRIVER_OK;
}

void driver_unregister(struct device_driver *driver)
{
	struct list *node;

	if (!driver || !driver->bus || !driver->registered)
		return;
	spinlock_acquire(&driver->bus->lock);
	driver->registered = 0;
	list_remove(&driver->node);
	spinlock_release(&driver->bus->lock);

	for (node = driver->bus->devices.next;
	     node != &driver->bus->devices; node = node->next) {
		struct device *device = list_entry(node, struct device, node);

		spinlock_acquire(&driver->bus->lock);
		if (device->driver != driver || device->state != DEVICE_BOUND) {
			spinlock_release(&driver->bus->lock);
			continue;
		}
		device->state = DEVICE_PROBING;
		spinlock_release(&driver->bus->lock);
		if (driver->remove)
			driver->remove(device);
		spinlock_acquire(&driver->bus->lock);
		device->driver = 0;
		device->state = DEVICE_UNBOUND;
		device->driver_data = 0;
		spinlock_release(&driver->bus->lock);
	}
}
