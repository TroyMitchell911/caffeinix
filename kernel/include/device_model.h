#ifndef __CAFFEINIX_KERNEL_DEVICE_MODEL_H
#define __CAFFEINIX_KERNEL_DEVICE_MODEL_H

#include <list.h>
#include <spinlock.h>
#include <typedefs.h>

struct device;
struct device_driver;
struct device_node;

enum device_state {
	DEVICE_UNBOUND,
	DEVICE_PROBING,
	DEVICE_BOUND,
};

enum driver_status {
	DRIVER_OK = 0,
	DRIVER_ERR_INVAL = -1,
	DRIVER_ERR_EXIST = -2,
	DRIVER_ERR_BUSY = -3,
	DRIVER_ERR_NODEV = -4,
};

struct bus_type {
	const char *name;
	int (*match)(struct device *device,
	             struct device_driver *driver);
	struct spinlock lock;
	struct list devices;
	struct list drivers;
	struct list node;
	uint8 registered;
};

struct device_driver {
	const char *name;
	struct bus_type *bus;
	int (*probe)(struct device *device);
	void (*remove)(struct device *device);
	struct list node;
	uint8 registered;
};

struct device {
	const char *name;
	struct bus_type *bus;
	struct device *parent;
	struct device_driver *driver;
	struct device_node *of_node;
	void *driver_data;
	void (*release)(struct device *device);
	struct list node;
	struct list children;
	struct list sibling;
	enum device_state state;
	uint32 refcount;
	uint8 registered;
};

void driver_core_init(void);
int bus_register(struct bus_type *bus);
int bus_unregister(struct bus_type *bus);
int device_register(struct device *device);
void device_unregister(struct device *device);
struct device *device_get(struct device *device);
void device_put(struct device *device);
int driver_register(struct device_driver *driver);
void driver_unregister(struct device_driver *driver);
int driver_core_selftest(void);

static inline void dev_set_drvdata(struct device *device, void *data)
{
	device->driver_data = data;
}

static inline void *dev_get_drvdata(struct device *device)
{
	return device->driver_data;
}

#endif
