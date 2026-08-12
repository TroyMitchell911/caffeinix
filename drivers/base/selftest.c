#include <device_model.h>
#include <mystring.h>

static int release_count;
static int remove_count;
static int probe_count;

static void test_release(struct device *device)
{
	(void)device;
	release_count++;
}

static int test_match(struct device *device, struct device_driver *driver)
{
	if (!strcmp(device->name, "early"))
		return !strcmp(driver->name, "early");
	if (!strcmp(device->name, "late"))
		return !strcmp(driver->name, "late");
	if (!strcmp(device->name, "retry"))
		return !strcmp(driver->name, "fail") ||
		       !strcmp(driver->name, "rescue");
	return 0;
}

static int test_probe(struct device *device)
{
	probe_count++;
	dev_set_drvdata(device, device);
	return 0;
}

static int test_fail_probe(struct device *device)
{
	probe_count++;
	dev_set_drvdata(device, device);
	return -1;
}

static void test_remove(struct device *device)
{
	if (dev_get_drvdata(device) == device)
		remove_count++;
}

int driver_core_selftest(void)
{
	struct bus_type bus = {
		.name = "selftest",
		.match = test_match,
	};
	struct device_driver early_driver = {
		.name = "early",
		.bus = &bus,
		.probe = test_probe,
		.remove = test_remove,
	};
	struct device_driver early_duplicate = {
		.name = "early",
		.bus = &bus,
	};
	struct device_driver late_driver = {
		.name = "late",
		.bus = &bus,
		.probe = test_probe,
		.remove = test_remove,
	};
	struct device_driver fail_driver = {
		.name = "fail",
		.bus = &bus,
		.probe = test_fail_probe,
	};
	struct device_driver rescue_driver = {
		.name = "rescue",
		.bus = &bus,
		.probe = test_probe,
		.remove = test_remove,
	};
	struct device early = {
		.name = "early",
		.bus = &bus,
		.release = test_release,
	};
	struct device early_duplicate_device = {
		.name = "early",
		.bus = &bus,
		.release = test_release,
	};
	struct device late = {
		.name = "late",
		.bus = &bus,
		.release = test_release,
	};
	struct device retry = {
		.name = "retry",
		.bus = &bus,
		.release = test_release,
	};
	struct device unmatched = {
		.name = "unmatched",
		.bus = &bus,
		.release = test_release,
	};
	struct device parent = {
		.name = "parent",
		.bus = &bus,
		.release = test_release,
	};
	struct device child = {
		.name = "child",
		.bus = &bus,
		.parent = &parent,
		.release = test_release,
	};

	release_count = 0;
	remove_count = 0;
	probe_count = 0;
	if (bus_register(&bus) || bus_register(&bus) != DRIVER_ERR_EXIST)
		return -1;
	if (driver_register(&early_driver) ||
	    driver_register(&early_duplicate) != DRIVER_ERR_EXIST)
		return -2;
	if (device_register(&early) || early.state != DEVICE_BOUND ||
	    early.driver != &early_driver || dev_get_drvdata(&early) != &early)
		return -3;
	if (device_register(&early_duplicate_device) != DRIVER_ERR_EXIST)
		return -4;
	if (device_register(&late) || late.state != DEVICE_UNBOUND ||
	    driver_register(&late_driver) || late.state != DEVICE_BOUND)
		return -5;
	if (driver_register(&fail_driver) || device_register(&retry) ||
	    retry.state != DEVICE_UNBOUND || retry.driver)
		return -6;
	if (driver_register(&rescue_driver) || retry.state != DEVICE_BOUND ||
	    retry.driver != &rescue_driver)
		return -7;
	if (device_register(&unmatched) || unmatched.state != DEVICE_UNBOUND)
		return -8;
	if (device_register(&parent) || device_register(&child) ||
	    parent.refcount != 2 || parent.children.next != &child.sibling)
		return -9;
	device_unregister(&parent);
	if (release_count || child.parent != &parent)
		return -10;
	device_unregister(&child);
	device_unregister(&early);
	device_unregister(&late);
	device_unregister(&retry);
	device_unregister(&unmatched);
	driver_unregister(&rescue_driver);
	driver_unregister(&fail_driver);
	driver_unregister(&late_driver);
	driver_unregister(&early_driver);
	if (bus_unregister(&bus) || probe_count != 4 || remove_count != 3 ||
	    release_count != 6)
		return -11;
	return 0;
}
