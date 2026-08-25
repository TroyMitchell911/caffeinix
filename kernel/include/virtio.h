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

void virtio_bus_init(void);
int virtio_mmio_init(void);
int virtio_rng_init(void);
int virtio_device_register(struct virtio_device *device);
void virtio_device_unregister(struct virtio_device *device);
int virtio_driver_register(struct virtio_driver *driver);
void virtio_driver_unregister(struct virtio_driver *driver);
int virtio_has_feature(struct virtio_device *device, uint32 feature);
void virtio_config_changed(struct virtio_device *device);
int virtio_find_vqs(struct virtio_device *device, uint16 count,
		    struct virtqueue **queues,
		    void (**callbacks)(struct virtqueue *queue),
		    const char *const names[]);

static inline struct virtio_device *to_virtio_device(
					struct device *device)
{
	return container_of(device, struct virtio_device, device);
}

static inline struct virtio_driver *to_virtio_driver(
					struct device_driver *driver)
{
	return container_of(driver, struct virtio_driver, driver);
}

#endif
