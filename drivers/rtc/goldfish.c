#include <device_model.h>
#include <goldfish_rtc.h>
#include <io.h>
#include <ktime.h>
#include <mystring.h>
#include <palloc.h>
#include <platform_device.h>
#include <printk.h>
#include <resource.h>

#define GOLDFISH_RTC_TIME_LOW  0x00
#define GOLDFISH_RTC_TIME_HIGH 0x04
#define GOLDFISH_RTC_REG_SIZE  0x08

struct goldfish_rtc {
	struct platform_device *platform;
	void *membase;
	uint64 mapsize;
};

static int goldfish_rtc_probe(struct platform_device *platform)
{
	struct goldfish_rtc *rtc;
	struct resource *resource;
	volatile uint8 *base;
	uint64 nanoseconds;
	uint32 low, high;

	resource = platform_get_resource(platform, RESOURCE_MEM, 0);
	if (!resource || resource_size(resource) < GOLDFISH_RTC_REG_SIZE)
		return DRIVER_ERR_NODEV;
	rtc = malloc(sizeof(*rtc));
	if (!rtc)
		return DRIVER_ERR_BUSY;
	memset(rtc, 0, sizeof(*rtc));
	rtc->platform = platform;
	rtc->mapsize = resource_size(resource);
	rtc->membase = ioremap(resource->start, rtc->mapsize);
	if (!rtc->membase) {
		free(rtc);
		return DRIVER_ERR_NODEV;
	}
	base = rtc->membase;
	/* Reading the low word latches the matching high word. */
	low = readl(base + GOLDFISH_RTC_TIME_LOW);
	high = readl(base + GOLDFISH_RTC_TIME_HIGH);
	nanoseconds = (uint64)high << 32 | low;
	ktime_set_realtime_ns(nanoseconds);
	dev_set_drvdata(&platform->device, rtc);
	pr_info("rtc: goldfish wall clock initialized");
	return DRIVER_OK;
}

static void goldfish_rtc_remove(struct platform_device *platform)
{
	struct goldfish_rtc *rtc = dev_get_drvdata(&platform->device);

	if (!rtc)
		return;
	iounmap(rtc->membase, rtc->mapsize);
	dev_set_drvdata(&platform->device, 0);
	free(rtc);
}

static const struct of_device_id goldfish_rtc_matches[] = {
	{ .compatible = "google,goldfish-rtc" },
	{ 0 },
};

static struct platform_driver goldfish_rtc_driver = {
	.driver = {
		.name = "goldfish-rtc",
	},
	.of_match_table = goldfish_rtc_matches,
	.probe = goldfish_rtc_probe,
	.remove = goldfish_rtc_remove,
};

int goldfish_rtc_init(void)
{
	return platform_driver_register(&goldfish_rtc_driver);
}
