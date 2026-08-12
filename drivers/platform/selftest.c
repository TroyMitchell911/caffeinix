#include <device_model.h>
#include <mystring.h>
#include <platform_device.h>
#include <resource.h>

static int platform_test_probes;
static int platform_test_removes;
static int platform_test_releases;

static int platform_test_probe(struct platform_device *device)
{
	struct resource *memory;
	int irq;

	memory = platform_get_resource(device, RESOURCE_MEM, 0);
	irq = platform_get_irq(device, 0);
	if (!memory || resource_size(memory) != 0x100 || irq <= 0 ||
	    !platform_get_match_data(device))
		return DRIVER_ERR_NODEV;
	platform_test_probes++;
	return DRIVER_OK;
}

static void platform_test_remove(struct platform_device *device)
{
	(void)device;
	platform_test_removes++;
}

static void platform_test_release(struct device *device)
{
	(void)device;
	platform_test_releases++;
}

static const struct of_device_id platform_test_matches[] = {
	{ .compatible = "caffeinix,platform-selftest" },
	{ 0 },
};

static struct platform_driver platform_test_driver = {
	.driver = {
		.name = "platform-selftest",
	},
	.of_match_table = platform_test_matches,
	.probe = platform_test_probe,
	.remove = platform_test_remove,
};

int platform_core_selftest(void)
{
	static struct platform_device devices[2];
	int registered = 0;
	int status = -1;

	memset(devices, 0, sizeof(devices));
	platform_test_probes = 0;
	platform_test_removes = 0;
	platform_test_releases = 0;
	if (platform_driver_register(&platform_test_driver) < 0)
		return -1;
	for (int index = 0; index < 2; index++) {
		devices[index].device.name = index ?
			"platform-test-1" : "platform-test-0";
		devices[index].device.release = platform_test_release;
		devices[index].compatible =
			"caffeinix,platform-selftest";
		devices[index].resources[0].start = 0x1000 + index * 0x100;
		devices[index].resources[0].end =
			devices[index].resources[0].start + 0xff;
		devices[index].resources[0].flags = RESOURCE_MEM;
		devices[index].resources[1].start = 80 + index;
		devices[index].resources[1].end = 80 + index;
		devices[index].resources[1].flags = RESOURCE_IRQ;
		devices[index].resource_count = 2;
		if (platform_device_register(&devices[index]) < 0)
			goto out;
		registered++;
	}
	if (platform_test_probes != 2 ||
	    devices[0].device.state != DEVICE_BOUND ||
	    devices[1].device.state != DEVICE_BOUND)
		goto out;
	status = 0;

out:
	while (registered)
		platform_device_unregister(&devices[--registered]);
	platform_driver_unregister(&platform_test_driver);
	if (platform_test_removes != 2 || platform_test_releases != 2)
		status = -1;
	return status;
}
