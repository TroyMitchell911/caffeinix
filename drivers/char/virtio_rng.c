#include <ktime.h>
#include <mystring.h>
#include <palloc.h>
#include <random.h>
#include <virtio.h>
#include <virtio_ring.h>
#include <workqueue.h>

#define VIRTIO_RNG_SEED_BYTES 64
#define VIRTIO_RNG_TIMEOUT_MS 100

struct virtio_rng {
	struct virtio_device *virtio;
	struct virtqueue *queue;
	struct work_struct work;
	uint8 seed[VIRTIO_RNG_SEED_BYTES];
	uint8 completed;
};

static void virtio_rng_consume(struct virtio_rng *rng)
{
	uint32 length;
	void *token;

	while ((token = virtqueue_get_used(rng->queue, &length))) {
		if (token != rng)
			continue;
		if (length > sizeof(rng->seed))
			length = sizeof(rng->seed);
		if (length)
			random_add_hardware(rng->seed, length);
		memset(rng->seed, 0, sizeof(rng->seed));
		__atomic_store_n(&rng->completed, 1, __ATOMIC_RELEASE);
	}
}

static void virtio_rng_work(struct work_struct *work)
{
	struct virtio_rng *rng = container_of(work, struct virtio_rng, work);

	virtio_rng_consume(rng);
}

static void virtio_rng_queue_done(struct virtqueue *queue)
{
	struct virtio_rng *rng = queue->private;

	if (rng)
		schedule_work(&rng->work);
}

static int virtio_rng_probe(struct virtio_device *device)
{
	static const char *const names[] = { "virtio-rng input" };
	void (*callbacks[])(struct virtqueue *) = { virtio_rng_queue_done };
	struct virtio_rng *rng;

	rng = calloc(1, sizeof(*rng));
	if (!rng)
		return DRIVER_ERR_BUSY;
	rng->virtio = device;
	work_init(&rng->work, virtio_rng_work);
	if (virtio_find_vqs(device, 1, &rng->queue, callbacks, names) < 0)
		goto failed;
	rng->queue->private = rng;
	dev_set_drvdata(&device->device, rng);
	return DRIVER_OK;

failed:
	if (rng->queue)
		device->config->del_vqs(device);
	free(rng);
	return DRIVER_ERR_NODEV;
}

static void virtio_rng_ready(struct virtio_device *device)
{
	struct virtio_rng *rng = dev_get_drvdata(&device->device);
	struct virtio_buffer buffer;
	uint64 start;

	if (!rng)
		return;
	buffer.address = rng->seed;
	buffer.length = sizeof(rng->seed);
	buffer.direction = DMA_FROM_DEVICE;
	if (virtqueue_add(rng->queue, &buffer, 1, rng) < 0)
		return;
	virtqueue_kick(rng->queue);
	start = ktime_get_ms();
	while (!__atomic_load_n(&rng->completed, __ATOMIC_ACQUIRE) &&
	       !virtqueue_has_used(rng->queue) &&
	       ktime_get_ms() - start < VIRTIO_RNG_TIMEOUT_MS)
		asm volatile("nop");
	virtio_rng_consume(rng);
}

static void virtio_rng_remove(struct virtio_device *device)
{
	struct virtio_rng *rng = dev_get_drvdata(&device->device);

	if (!rng)
		return;
	cancel_work_sync(&rng->work);
	virtio_rng_consume(rng);
	(void)virtqueue_detach_unused(rng->queue);
	dev_set_drvdata(&device->device, 0);
	free(rng);
}

static const struct virtio_device_id virtio_rng_ids[] = {
	{ VIRTIO_ID_RNG, VIRTIO_DEV_ANY_ID },
	{ 0 },
};

static struct virtio_driver virtio_rng_driver = {
	.driver = {
		.name = "virtio-rng",
	},
	.id_table = virtio_rng_ids,
	.probe = virtio_rng_probe,
	.ready = virtio_rng_ready,
	.remove = virtio_rng_remove,
};

int virtio_rng_init(void)
{
	return virtio_driver_register(&virtio_rng_driver);
}
