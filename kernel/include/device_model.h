/*
 * Caffeinix device-model interfaces.
 *
 * Buses own registration and matching; devices retain their release callback
 * until their final reference is dropped.  Probe and remove callbacks run
 * outside the bus lock, so they may sleep but must tolerate unregistration.
 */
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
	uint64 dma_mask;
	void (*release)(struct device *device);
	struct list node;
	struct list children;
	struct list sibling;
	enum device_state state;
	uint32 refcount;
	uint8 registered;
};

/**
 * driver_core_init() - Initialize the global bus registry.
 *
 * Context:
 * Early process context before bus or device registration.  Does not
 * sleep and must run exactly once.
 */
void driver_core_init(void);
/**
 * bus_register() - Publish an empty bus for device and driver registration.
 * @bus: Caller-owned bus with a stable name and match callback.
 *
 * Context:
 * Process context; takes the global registry lock but does not sleep.
 * Return:
 * %DRIVER_OK, %DRIVER_ERR_INVAL for an invalid bus, or
 * %DRIVER_ERR_EXIST for a duplicate registration or name.
 */
int bus_register(struct bus_type *bus);
/**
 * bus_unregister() - Remove an otherwise empty bus from the registry.
 * @bus: Registered bus with no devices or drivers.
 *
 * Context:
 * Process context; does not sleep.  The caller owns @bus storage.
 * Return:
 * %DRIVER_OK, %DRIVER_ERR_BUSY when members remain, or an invalid
 * registration error.
 */
int bus_unregister(struct bus_type *bus);
/**
 * device_register() - Add a device and probe matching drivers.
 * @device: Caller-owned device with name, bus, and release callback.
 *
 * The core takes the initial reference and may invoke probe without bus locks.
 * Context:
 * Process context; probe may sleep.  @device must remain allocated
 * until release is called after device_unregister() and the final device_put().
 * Return:
 * %DRIVER_OK or a registration error.
 */
int device_register(struct device *device);
/**
 * device_unregister() - Remove a device and invoke its bound driver remove.
 * @device: Registered device whose storage remains valid through release.
 *
 * Context:
 * Process context; remove may sleep.  Drops the registration
 * reference, but outstanding references keep @device alive.
 */
void device_unregister(struct device *device);
/**
 * device_get() - Acquire a reference to a registered device.
 * @device: Device whose storage and bus remain live through the call, or NULL.
 *
 * Context: Caller excludes device_unregister() and bus teardown during this
 * acquisition; takes the bus spinlock without sleeping, so the caller must
 * not hold that lock. The registration check precedes locking and is not a
 * lookup primitive safe against concurrent teardown. After acquisition the
 * reference pins device storage, not registration or the bound driver; using
 * driver state still requires lifecycle exclusion.
 * Return: @device with one additional reference to release with device_put(),
 * or %NULL for NULL, unregistered, or zero-reference input.
 */
struct device *device_get(struct device *device);
/**
 * device_put() - Release a reference acquired by device_get().
 * @device: Referenced device, or %NULL.
 *
 * Context:
 * Process context; the final put calls release outside the bus lock,
 * and that callback may free the device.
 */
void device_put(struct device *device);
/**
 * driver_register() - Register a driver and probe existing matching devices.
 * @driver: Caller-owned driver with a bus and optional callbacks.
 *
 * Context:
 * Process context; probe callbacks may sleep and run unlocked.
 * Return:
 * %DRIVER_OK or a registration error.
 */
int driver_register(struct device_driver *driver);
/**
 * driver_unregister() - Unbind and remove a registered driver.
 * @driver: Registered driver whose storage stays valid until this returns.
 *
 * Context:
 * Process context; remove callbacks may sleep.
 */
void driver_unregister(struct device_driver *driver);
/**
 * driver_core_selftest() - Exercise device-model registration invariants.
 *
 * Context:
 * Early process context; test-only and not safe with live users.
 * Return:
 * Zero on success, negative on failure.
 */
int driver_core_selftest(void);

/**
 * dev_set_drvdata() - Associate private driver state with a device.
 * @device: Registered or probing device.
 * @data: Driver-owned state, or %NULL.
 *
 * Context:
 * Caller serializes access with its probe/remove or bus protocol.
 */
static inline void dev_set_drvdata(struct device *device, void *data)
{
	device->driver_data = data;
}

/**
 * dev_get_drvdata() - Return private state associated with a device.
 * @device: Device previously passed to dev_set_drvdata().
 *
 * Context:
 * Caller must ensure @device and the returned state remain alive.
 * Return:
 * The stored pointer, possibly %NULL.
 */
static inline void *dev_get_drvdata(struct device *device)
{
	return device->driver_data;
}

#endif
