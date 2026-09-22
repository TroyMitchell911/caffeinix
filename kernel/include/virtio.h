/* VirtIO device-model bus and transport-independent feature interface. */
#ifndef __CAFFEINIX_KERNEL_VIRTIO_H
#define __CAFFEINIX_KERNEL_VIRTIO_H

#include <device_model.h>
#include <typedefs.h>

#define VIRTIO_ID_NET 1
#define VIRTIO_ID_BLOCK 2
#define VIRTIO_ID_RNG 4

#define VIRTIO_F_VERSION_1 32

#define VIRTIO_CONFIG_S_ACKNOWLEDGE 1
#define VIRTIO_CONFIG_S_DRIVER 2
#define VIRTIO_CONFIG_S_DRIVER_OK 4
#define VIRTIO_CONFIG_S_FEATURES_OK 8
#define VIRTIO_CONFIG_S_DEVICE_NEEDS_RESET 64
#define VIRTIO_CONFIG_S_FAILED 128

struct virtio_device;
struct virtqueue;

struct virtio_device_id {
	uint32 device;
	uint32 vendor;
};

#define VIRTIO_DEV_ANY_ID (~0U)

struct virtio_config_ops {
	uint64 (*get_features)(struct virtio_device *device);
	int (*finalize_features)(struct virtio_device *device);
	int (*find_vqs)(struct virtio_device *device, uint16 count,
			struct virtqueue **queues,
			void (**callbacks)(struct virtqueue *queue),
			const char *const names[]);
	void (*del_vqs)(struct virtio_device *device);
	void (*reset)(struct virtio_device *device);
	uint8 (*get_status)(struct virtio_device *device);
	void (*set_status)(struct virtio_device *device, uint8 status);
	void (*get_config)(struct virtio_device *device, uint32 offset,
			   void *buffer, uint32 length);
	void (*set_config)(struct virtio_device *device, uint32 offset,
			   const void *buffer, uint32 length);
};

struct virtio_device {
	struct device device;
	uint32 id;
	uint32 vendor;
	uint32 transport_index;
	uint64 features;
	const struct virtio_config_ops *config;
	void *private;
};

struct virtio_driver {
	struct device_driver driver;
	const struct virtio_device_id *id_table;
	uint64 feature_table;
	int (*probe)(struct virtio_device *device);
	void (*ready)(struct virtio_device *device);
	void (*config_changed)(struct virtio_device *device);
	void (*remove)(struct virtio_device *device);
};

/**
 * virtio_bus_init() - Register the generic VirtIO bus.
 *
 * Context:
 * Early boot before transport device registration; does not sleep.
 */
void virtio_bus_init(void);
/**
 * virtio_mmio_init() - Register the VirtIO MMIO platform transport driver.
 *
 * Context:
 * Boot process context; may probe FDT devices.
 * Return:
 * Zero or driver registration error.
 */
int virtio_mmio_init(void);
/**
 * virtio_rng_init() - Register the VirtIO entropy driver.
 *
 * Context:
 * Boot process context; may probe devices.
 * Return:
 * Zero or driver registration error.
 */
int virtio_rng_init(void);
/**
 * virtio_device_register() - Register a transport-discovered VirtIO device.
 * @device: Caller-owned transport device with config operations and ID.
 *
 * Context:
 * Process context; matching driver probe may sleep.
 * Return:
 * Device-model status.
 */
int virtio_device_register(struct virtio_device *device);
/**
 * virtio_device_unregister() - Remove a VirtIO transport device.
 * @device: Registered device, or %NULL.
 *
 * Context:
 * Process context; driver removal may sleep.
 */
void virtio_device_unregister(struct virtio_device *device);
/**
 * virtio_driver_register() - Register a functional VirtIO driver.
 * @driver: Driver with ID and feature tables.
 *
 * Context:
 * Process context; probe may sleep.
 * Return:
 * Device-model status.
 */
int virtio_driver_register(struct virtio_driver *driver);
/**
 * virtio_driver_unregister() - Unbind and remove a VirtIO driver.
 * @driver: Registered driver, or %NULL.
 *
 * Context:
 * Process context; remove may sleep.
 */
void virtio_driver_unregister(struct virtio_driver *driver);
/**
 * virtio_has_feature() - Test a negotiated feature bit.
 * @device: Bound VirtIO device.
 * @feature: Feature bit number below 64.
 *
 * Context:
 * Any context after negotiation.
 * Return:
 * Non-zero when feature was accepted.
 */
int virtio_has_feature(struct virtio_device *device, uint32 feature);
/**
 * virtio_config_changed() - Deliver a transport configuration notification.
 * @device: Bound VirtIO device.
 *
 * Context:
 * IRQ-safe only when the driver callback is IRQ-safe; current MMIO
 * transport invokes it from its interrupt handler.
 */
void virtio_config_changed(struct virtio_device *device);
/*
 * virtio_find_vqs() - Ask a transport to allocate and bind virtqueues.
 * @device: Bound VirtIO device.
 * @count: Number of requested queues.
 * @queues: Receives queue pointers.
 * @callbacks: Per-queue IRQ callbacks, or %NULL entries.
 * @names: Stable per-queue diagnostic names.
 *
 * Context:
 * Process context; may allocate.  Driver must later delete queues.
 * Return:
 * Zero or a negative transport error.
 */
int virtio_find_vqs(struct virtio_device *device, uint16 count,
		    struct virtqueue **queues,
		    void (**callbacks)(struct virtqueue *queue),
		    const char *const names[]);

/**
 * to_virtio_device() - Convert embedded generic device storage.
 * @device: Address of struct virtio_device::device.
 *
 * Context:
 * Any context; caller guarantees object type.
 * Return:
 * Containing VirtIO device.
 */
static inline struct virtio_device *to_virtio_device(
					struct device *device)
{
	return container_of(device, struct virtio_device, device);
}

/**
 * to_virtio_driver() - Convert embedded generic driver storage.
 * @driver: Address of struct virtio_driver::driver.
 *
 * Context:
 * Any context; caller guarantees object type.
 * Return:
 * Containing VirtIO driver.
 */
static inline struct virtio_driver *to_virtio_driver(
					struct device_driver *driver)
{
	return container_of(driver, struct virtio_driver, driver);
}

#endif
