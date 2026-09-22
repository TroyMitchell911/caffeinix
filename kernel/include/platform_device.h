/*
 * Device-tree backed platform bus interface.
 *
 * Platform devices are constructed once from available FDT nodes.  Their
 * resource arrays and match data remain valid until device release.
 */
#ifndef __CAFFEINIX_KERNEL_PLATFORM_DEVICE_H
#define __CAFFEINIX_KERNEL_PLATFORM_DEVICE_H

#include <device_model.h>
#include <resource.h>

#define PLATFORM_RESOURCE_MAX 6

struct of_device_id {
	const char *compatible;
	const void *data;
};

struct platform_device {
	struct device device;
	struct resource resources[PLATFORM_RESOURCE_MAX];
	uint32 resource_count;
	const char *compatible;
	int id;
};

struct platform_driver {
	struct device_driver driver;
	const struct of_device_id *of_match_table;
	int (*probe)(struct platform_device *device);
	void (*remove)(struct platform_device *device);
};

/**
 * platform_bus_init() - Register the platform bus.
 *
 * Context:
 * Early boot before platform enumeration; does not sleep.
 */
void platform_bus_init(void);
/**
 * platform_device_register() - Register one caller-owned platform device.
 * @device: Device with stable resources and compatible string.
 *
 * Context:
 * Process context; its matching probe may sleep.
 * Return:
 * Device-model status.
 */
int platform_device_register(struct platform_device *device);
/**
 * platform_device_unregister() - Remove a platform device.
 * @device: Registered platform device, or %NULL.
 *
 * Context:
 * Process context; bound remove callbacks may sleep.
 */
void platform_device_unregister(struct platform_device *device);
/**
 * platform_driver_register() - Register a driver with FDT match entries.
 * @driver: Driver with a NULL-terminated of_match_table.
 *
 * Context:
 * Process context; probe may sleep.
 * Return:
 * Device-model status.
 */
int platform_driver_register(struct platform_driver *driver);
/**
 * platform_driver_unregister() - Unbind and unregister a platform driver.
 * @driver: Registered driver, or %NULL.
 *
 * Context:
 * Process context; remove callbacks may sleep.
 */
void platform_driver_unregister(struct platform_driver *driver);
/**
 * of_platform_populate() - Create platform devices for available FDT nodes.
 *
 * Context:
 * Boot process context; allocates device storage and may probe.
 * Return:
 * Zero on complete enumeration, negative on malformed FDT or failure.
 */
int of_platform_populate(void);
/**
 * platform_get_resource() - Find a typed resource by ordinal.
 * @device: Registered platform device.
 * @type: %RESOURCE_MEM or %RESOURCE_IRQ.
 * @index: Zero-based ordinal among resources of @type.
 *
 * Context:
 * Any context while @device is alive.
 * Return:
 * Matching resource, or %NULL.
 */
struct resource *platform_get_resource(struct platform_device *device,
				       uint32 type, uint32 index);
/**
 * platform_get_irq() - Return an IRQ resource by ordinal.
 * @device: Registered platform device.
 * @index: Zero-based IRQ resource ordinal.
 *
 * Context:
 * Any context while @device is alive.
 * Return:
 * Positive IRQ number, or negative when absent.
 */
int platform_get_irq(struct platform_device *device, uint32 index);
/**
 * platform_get_match_data() - Return data from the matched OF entry.
 * @device: Bound platform device.
 *
 * Context:
 * Any context while binding remains stable.
 * Return:
 * Matched table entry, or %NULL when unbound or unmatched.
 */
const struct of_device_id *platform_get_match_data(
				struct platform_device *device);
/**
 * platform_core_selftest() - Verify platform matching and resource lookup.
 *
 * Context:
 * Test-only process context; must run before production enumeration.
 * Return:
 * Zero on success, negative on failure.
 */
int platform_core_selftest(void);

/**
 * to_platform_device() - Convert an embedded generic device.
 * @device: Address of struct platform_device::device.
 *
 * Context:
 * Any context; caller guarantees the containing object type.
 * Return:
 * Containing platform device.
 */
static inline struct platform_device *to_platform_device(
					struct device *device)
{
	return container_of(device, struct platform_device, device);
}

/**
 * to_platform_driver() - Convert an embedded generic driver.
 * @driver: Address of struct platform_driver::driver.
 *
 * Context:
 * Any context; caller guarantees the containing object type.
 * Return:
 * Containing platform driver.
 */
static inline struct platform_driver *to_platform_driver(
					struct device_driver *driver)
{
	return container_of(driver, struct platform_driver, driver);
}

#endif
