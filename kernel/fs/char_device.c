#include <char_device.h>
#include <debug.h>
#include <device.h>
#include <mystring.h>
#include <process.h>
#include <spinlock.h>

#define CHAR_REGION_MAX 16
#define CHAR_DEVICE_MAX 32
#define CHAR_NODE_MAX 32

struct char_region {
	uint64 first;
	uint32 count;
	const char *name;
	int registered;
};

static struct {
	struct spinlock lock;
	struct char_region regions[CHAR_REGION_MAX];
	struct char_device *devices[CHAR_DEVICE_MAX];
	struct char_device_node nodes[CHAR_NODE_MAX];
	uint64 next_inode;
} char_devices;

static struct char_device null_device;
static struct char_device zero_device;
static char zero_page[PGSIZE];

static int range_valid(uint64 first, uint32 count)
{
	uint32 minor = VFS_DEVICE_MINOR(first);

	return count && minor <= 0xffffffffU - (count - 1);
}

static int ranges_overlap(uint64 left, uint32 left_count, uint64 right,
			  uint32 right_count)
{
	uint64 left_end, left_minor, right_end, right_minor;

	if (VFS_DEVICE_MAJOR(left) != VFS_DEVICE_MAJOR(right))
		return 0;
	left_minor = VFS_DEVICE_MINOR(left);
	right_minor = VFS_DEVICE_MINOR(right);
	left_end = left_minor + left_count;
	right_end = right_minor + right_count;
	return left_minor < right_end && right_minor < left_end;
}

int char_device_region_register(uint64 first, uint32 count,
				const char *name)
{
	struct char_region *free_region = 0;
	uint32 i;

	if (!range_valid(first, count) || !name)
		return VFS_ERR_INVAL;
	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_REGION_MAX; i++) {
		struct char_region *region = &char_devices.regions[i];

		if (!region->registered) {
			if (!free_region)
				free_region = region;
			continue;
		}
		if (ranges_overlap(first, count, region->first, region->count)) {
			spinlock_release(&char_devices.lock);
			return VFS_ERR_BUSY;
		}
	}
	if (!free_region) {
		spinlock_release(&char_devices.lock);
		return VFS_ERR_NOSPC;
	}
	free_region->first = first;
	free_region->count = count;
	free_region->name = name;
	free_region->registered = 1;
	spinlock_release(&char_devices.lock);
	return VFS_OK;
}

int char_device_region_unregister(uint64 first, uint32 count)
{
	struct char_region *region = 0;
	uint32 i;

	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_DEVICE_MAX; i++) {
		struct char_device *device = char_devices.devices[i];

		if (device && ranges_overlap(first, count, device->device,
		                             device->count)) {
			spinlock_release(&char_devices.lock);
			return VFS_ERR_BUSY;
		}
	}
	for (i = 0; i < CHAR_REGION_MAX; i++) {
		if (char_devices.regions[i].registered &&
		    char_devices.regions[i].first == first &&
		    char_devices.regions[i].count == count) {
			region = &char_devices.regions[i];
			break;
		}
	}
	if (!region) {
		spinlock_release(&char_devices.lock);
		return VFS_ERR_NOENT;
	}
	memset(region, 0, sizeof(*region));
	spinlock_release(&char_devices.lock);
	return VFS_OK;
}

int char_device_add(struct char_device *device, uint64 first, uint32 count)
{
	struct char_region *owner = 0;
	uint64 first_minor = VFS_DEVICE_MINOR(first);
	uint32 i, free_slot = CHAR_DEVICE_MAX;

	if (!device || !device->operations || !range_valid(first, count))
		return VFS_ERR_INVAL;
	spinlock_acquire(&char_devices.lock);
	if (device->registered) {
		spinlock_release(&char_devices.lock);
		return VFS_ERR_EXIST;
	}
	for (i = 0; i < CHAR_REGION_MAX; i++) {
		struct char_region *region = &char_devices.regions[i];
		uint64 region_minor;

		if (!region->registered ||
		    VFS_DEVICE_MAJOR(region->first) != VFS_DEVICE_MAJOR(first))
			continue;
		region_minor = VFS_DEVICE_MINOR(region->first);
		if (first_minor >= region_minor &&
		    first_minor + count <= region_minor + region->count) {
			owner = region;
			break;
		}
	}
	if (!owner) {
		spinlock_release(&char_devices.lock);
		return VFS_ERR_NODEV;
	}
	for (i = 0; i < CHAR_DEVICE_MAX; i++) {
		if (!char_devices.devices[i]) {
			if (free_slot == CHAR_DEVICE_MAX)
				free_slot = i;
			continue;
		}
		if (ranges_overlap(first, count, char_devices.devices[i]->device,
		                   char_devices.devices[i]->count)) {
			spinlock_release(&char_devices.lock);
			return VFS_ERR_BUSY;
		}
	}
	if (free_slot == CHAR_DEVICE_MAX) {
		spinlock_release(&char_devices.lock);
		return VFS_ERR_NOSPC;
	}
	device->device = first;
	device->count = count;
	device->registered = 1;
	char_devices.devices[free_slot] = device;
	spinlock_release(&char_devices.lock);
	return VFS_OK;
}

int char_device_remove(struct char_device *device)
{
	uint32 i;

	if (!device)
		return VFS_ERR_INVAL;
	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_NODE_MAX; i++) {
		if (char_devices.nodes[i].name[0] &&
		    ranges_overlap(device->device, device->count,
		                   char_devices.nodes[i].device, 1)) {
			spinlock_release(&char_devices.lock);
			return VFS_ERR_BUSY;
		}
	}
	for (i = 0; i < CHAR_DEVICE_MAX; i++) {
		if (char_devices.devices[i] != device)
			continue;
		char_devices.devices[i] = 0;
		device->registered = 0;
		spinlock_release(&char_devices.lock);
		return VFS_OK;
	}
	spinlock_release(&char_devices.lock);
	return VFS_ERR_NOENT;
}

struct char_device *char_device_lookup(uint64 number)
{
	struct char_device *result = 0;
	uint32 minor = VFS_DEVICE_MINOR(number);
	uint32 i;

	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_DEVICE_MAX; i++) {
		struct char_device *device = char_devices.devices[i];
		uint32 first_minor;

		if (!device ||
		    VFS_DEVICE_MAJOR(device->device) != VFS_DEVICE_MAJOR(number))
			continue;
		first_minor = VFS_DEVICE_MINOR(device->device);
		if (minor >= first_minor && minor < first_minor + device->count) {
			result = device;
			break;
		}
	}
	spinlock_release(&char_devices.lock);
	return result;
}

int char_device_is_terminal(uint64 number)
{
	struct char_device *device = char_device_lookup(number);

	return device && (device->flags & CHAR_DEVICE_TERMINAL);
}

int char_device_node_register(const char *name, uint64 device, uint32 mode)
{
	struct char_device_node *free_node = 0;
	uint32 i;

	if (!name || !*name || strlen(name) > CHAR_DEVICE_NAME_MAX ||
	    !char_device_lookup(device))
		return VFS_ERR_INVAL;
	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_NODE_MAX; i++) {
		struct char_device_node *node = &char_devices.nodes[i];

		if (!node->name[0]) {
			if (!free_node)
				free_node = node;
			continue;
		}
		if (!strcmp(node->name, name)) {
			spinlock_release(&char_devices.lock);
			return VFS_ERR_EXIST;
		}
	}
	if (!free_node) {
		spinlock_release(&char_devices.lock);
		return VFS_ERR_NOSPC;
	}
	safe_strncpy(free_node->name, name, sizeof(free_node->name));
	free_node->device = device;
	free_node->mode = mode;
	free_node->inode_number = char_devices.next_inode++;
	spinlock_release(&char_devices.lock);
	return VFS_OK;
}

int char_device_node_unregister(const char *name)
{
	uint32 i;

	if (!name)
		return VFS_ERR_INVAL;
	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_NODE_MAX; i++) {
		if (strcmp(char_devices.nodes[i].name, name))
			continue;
		memset(&char_devices.nodes[i], 0,
		       sizeof(char_devices.nodes[i]));
		spinlock_release(&char_devices.lock);
		return VFS_OK;
	}
	spinlock_release(&char_devices.lock);
	return VFS_ERR_NOENT;
}

int char_device_node_find(const char *name, struct char_device_node *result)
{
	uint32 i;

	if (!name || !result)
		return VFS_ERR_INVAL;
	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_NODE_MAX; i++) {
		if (strcmp(char_devices.nodes[i].name, name))
			continue;
		*result = char_devices.nodes[i];
		spinlock_release(&char_devices.lock);
		return VFS_OK;
	}
	spinlock_release(&char_devices.lock);
	return VFS_ERR_NOENT;
}

int char_device_node_get(uint32 index, struct char_device_node *result)
{
	uint32 i, current = 0;

	if (!result)
		return VFS_ERR_INVAL;
	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_NODE_MAX; i++) {
		if (!char_devices.nodes[i].name[0])
			continue;
		if (current++ != index)
			continue;
		*result = char_devices.nodes[i];
		spinlock_release(&char_devices.lock);
		return VFS_OK;
	}
	spinlock_release(&char_devices.lock);
	return VFS_ERR_NOENT;
}

uint32 char_device_node_count(void)
{
	uint32 count = 0, i;

	spinlock_acquire(&char_devices.lock);
	for (i = 0; i < CHAR_NODE_MAX; i++)
		count += char_devices.nodes[i].name[0] != 0;
	spinlock_release(&char_devices.lock);
	return count;
}

static int64 null_read(struct char_device *device, struct vfs_file *file,
		       int user_destination, uint64 destination, uint64 count)
{
	(void)device;
	(void)file;
	(void)user_destination;
	(void)destination;
	(void)count;
	return 0;
}

static int64 null_write(struct char_device *device, struct vfs_file *file,
			int user_source, uint64 source, uint64 count)
{
	(void)device;
	(void)file;
	(void)user_source;
	(void)source;
	return count;
}

static int64 zero_read(struct char_device *device, struct vfs_file *file,
		       int user_destination, uint64 destination, uint64 count)
{
	uint64 total = 0;

	(void)device;
	(void)file;
	while (total < count) {
		uint64 remaining = count - total;
		uint32 chunk = remaining > PGSIZE ? PGSIZE : remaining;

		if (either_copyout(user_destination, destination + total,
		                   zero_page, chunk) < 0)
			return total ? total : VFS_ERR_FAULT;
		total += chunk;
	}
	return total;
}

static const struct char_device_operations null_operations = {
	.read = null_read,
	.write = null_write,
};

static const struct char_device_operations zero_operations = {
	.read = zero_read,
	.write = null_write,
};

void char_device_init(void)
{
	spinlock_init(&char_devices.lock, "character devices");
	char_devices.next_inode = 2;
	null_device.operations = &null_operations;
	zero_device.operations = &zero_operations;
	if (char_device_region_register(DEVICE_NULL, 3, "memory") < 0 ||
	    char_device_add(&null_device, DEVICE_NULL, 1) < 0 ||
	    char_device_add(&zero_device, DEVICE_ZERO, 1) < 0 ||
	    char_device_node_register("null", DEVICE_NULL, 0666) < 0 ||
	    char_device_node_register("zero", DEVICE_ZERO, 0666) < 0)
		PANIC("register memory character devices");
}
