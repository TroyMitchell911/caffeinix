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

void platform_bus_init(void);
int platform_device_register(struct platform_device *device);
void platform_device_unregister(struct platform_device *device);
int platform_driver_register(struct platform_driver *driver);
void platform_driver_unregister(struct platform_driver *driver);
int of_platform_populate(void);
struct resource *platform_get_resource(struct platform_device *device,
				       uint32 type, uint32 index);
int platform_get_irq(struct platform_device *device, uint32 index);
const struct of_device_id *platform_get_match_data(
				struct platform_device *device);
int platform_core_selftest(void);

static inline struct platform_device *to_platform_device(
					struct device *device)
{
	return container_of(device, struct platform_device, device);
}

static inline struct platform_driver *to_platform_driver(
					struct device_driver *driver)
{
	return container_of(driver, struct platform_driver, driver);
}

#endif
