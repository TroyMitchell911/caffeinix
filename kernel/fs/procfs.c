#include <debug.h>
#include <cpu.h>
#include <kernel_config.h>
#include <ksocket.h>
#include <ktime.h>
#include <loadavg.h>
#include <mystring.h>
#include <netdevice.h>
#include <network_stack.h>
#include <palloc.h>
#include <printf.h>
#include <process.h>
#include <procfs.h>
#include <riscv.h>
#include <scheduler.h>
#include <spinlock.h>
#include <stdarg.h>
#include <timer.h>
#include <trap.h>
#include <thread.h>
#include <vfs.h>
#include <wait.h>

#define PROCFS_OPEN_ORDER 1
#define PROCFS_OPEN_SIZE  (PGSIZE << PROCFS_OPEN_ORDER)
#define PROCFS_USER_HZ    100

enum procfs_kind {
	PROCFS_ROOT,
	PROCFS_SELF,
	PROCFS_PID_DIRECTORY,
	PROCFS_PID_STAT,
	PROCFS_PID_STATUS,
	PROCFS_PID_CMDLINE,
	PROCFS_MOUNTS,
	PROCFS_MEMINFO,
	PROCFS_UPTIME,
	PROCFS_STAT,
	PROCFS_LOADAVG,
	PROCFS_NET_DIRECTORY,
	PROCFS_NET_DEV,
	PROCFS_NET_ROUTE,
	PROCFS_NET_TCP,
	PROCFS_NET_UDP,
};

struct procfs_node {
	enum procfs_kind kind;
	int pid;
};

struct procfs_open {
	uint32 length;
	uint32 pid_count;
	int pids[NPROC];
	char data[];
};

struct procfs_buffer {
	char *data;
	uint32 length;
	uint32 capacity;
	int overflow;
};

_Static_assert(sizeof(struct procfs_open) <= PROCFS_OPEN_SIZE,
	       "procfs open state exceeds its allocation");

static const struct vfs_inode_operations procfs_inode_operations;
static const struct vfs_file_operations procfs_file_operations;
static const struct vfs_file_operations procfs_directory_operations;

static void procfs_emit(int character, void *context)
{
	struct procfs_buffer *buffer = context;

	if (buffer->length >= buffer->capacity) {
		buffer->overflow = 1;
		return;
	}
	buffer->data[buffer->length++] = character;
}

static void procfs_printf(struct procfs_buffer *buffer,
			  const char *format, ...)
{
	va_list arguments;

	va_start(arguments, format);
	vprintf_emit(procfs_emit, buffer, format, arguments);
	va_end(arguments);
}

static int procfs_parse_pid(const char *name, int *pid)
{
	uint64 value = 0;

	if (!name || !*name)
		return -1;
	while (*name) {
		if (*name < '0' || *name > '9' ||
		    value > (0x7fffffffU - (*name - '0')) / 10)
			return -1;
		value = value * 10 + *name++ - '0';
	}
	if (!value)
		return -1;
	*pid = value;
	return 0;
}

static uint64 procfs_inode_number(enum procfs_kind kind, int pid)
{
	if (kind == PROCFS_ROOT)
		return 1;
	return ((uint64)(uint32)pid << 8) + kind + 2;
}

static uint32 procfs_format_unsigned(char *buffer, uint32 size,
				     uint64 value)
{
	char reverse[24];
	uint32 length = 0, output = 0;

	do {
		reverse[length++] = '0' + value % 10;
		value /= 10;
	} while (value && length < sizeof(reverse));
	while (length && output < size)
		buffer[output++] = reverse[--length];
	return output;
}

static struct vfs_inode *procfs_wrap(struct vfs_super_block *superblock,
				     enum procfs_kind kind, int pid)
{
	struct procfs_node *node;
	struct vfs_inode *inode;

	node = malloc(sizeof(*node));
	if (!node)
		return 0;
	inode = vfs_inode_alloc(superblock);
	if (!inode) {
		free(node);
		return 0;
	}
	node->kind = kind;
	node->pid = pid;
	inode->private = node;
	inode->number = procfs_inode_number(kind, pid);
	inode->operations = &procfs_inode_operations;
	inode->nlink = 1;
	if (kind == PROCFS_ROOT || kind == PROCFS_PID_DIRECTORY ||
	    kind == PROCFS_NET_DIRECTORY) {
		inode->type = VFS_INODE_DIRECTORY;
		inode->mode = 0555;
		inode->nlink = 2;
		inode->file_operations = &procfs_directory_operations;
	} else if (kind == PROCFS_SELF) {
		inode->type = VFS_INODE_SYMLINK;
		inode->mode = 0777;
	} else {
		inode->type = VFS_INODE_REGULAR;
		inode->mode = 0444;
		inode->file_operations = &procfs_file_operations;
	}
	return inode;
}

static int procfs_lookup_root(struct vfs_inode *directory,
			      const char *name,
			      struct vfs_inode **result)
{
	static const struct {
		const char *name;
		enum procfs_kind kind;
	} fixed[] = {
		{ "self", PROCFS_SELF },
		{ "mounts", PROCFS_MOUNTS },
		{ "meminfo", PROCFS_MEMINFO },
		{ "uptime", PROCFS_UPTIME },
		{ "stat", PROCFS_STAT },
		{ "loadavg", PROCFS_LOADAVG },
		{ "net", PROCFS_NET_DIRECTORY },
	};
	struct process_snapshot snapshot;
	int pid, index;

	for (index = 0; index < (int)NELEM(fixed); index++) {
		if (!strcmp(name, fixed[index].name)) {
			*result = procfs_wrap(directory->superblock,
					      fixed[index].kind, 0);
			return *result ? VFS_OK : VFS_ERR_NOMEM;
		}
	}
	if (procfs_parse_pid(name, &pid) < 0 ||
	    process_snapshot_pid(pid, &snapshot, 0, 0, 0) < 0)
		return VFS_ERR_NOENT;
	*result = procfs_wrap(directory->superblock,
			      PROCFS_PID_DIRECTORY, pid);
	return *result ? VFS_OK : VFS_ERR_NOMEM;
}

static int procfs_lookup_pid(struct vfs_inode *directory,
			     const char *name,
			     struct vfs_inode **result)
{
	static const struct {
		const char *name;
		enum procfs_kind kind;
	} entries[] = {
		{ "stat", PROCFS_PID_STAT },
		{ "status", PROCFS_PID_STATUS },
		{ "cmdline", PROCFS_PID_CMDLINE },
	};
	struct procfs_node *node = directory->private;
	struct process_snapshot snapshot;
	int index;

	if (process_snapshot_pid(node->pid, &snapshot, 0, 0, 0) < 0)
		return VFS_ERR_NOENT;
	for (index = 0; index < (int)NELEM(entries); index++) {
		if (!strcmp(name, entries[index].name)) {
			*result = procfs_wrap(directory->superblock,
					      entries[index].kind,
					      node->pid);
			return *result ? VFS_OK : VFS_ERR_NOMEM;
		}
	}
	return VFS_ERR_NOENT;
}

static int procfs_lookup_net(struct vfs_inode *directory,
			     const char *name,
			     struct vfs_inode **result)
{
	static const struct {
		const char *name;
		enum procfs_kind kind;
	} entries[] = {
		{ "dev", PROCFS_NET_DEV },
		{ "route", PROCFS_NET_ROUTE },
		{ "tcp", PROCFS_NET_TCP },
		{ "udp", PROCFS_NET_UDP },
	};
	int index;

	for (index = 0; index < (int)NELEM(entries); index++) {
		if (!strcmp(name, entries[index].name)) {
			*result = procfs_wrap(directory->superblock,
					      entries[index].kind, 0);
			return *result ? VFS_OK : VFS_ERR_NOMEM;
		}
	}
	return VFS_ERR_NOENT;
}

static int procfs_lookup(struct vfs_inode *directory, const char *name,
			 struct vfs_inode **result)
{
	struct procfs_node *node = directory->private;

	if (!node || directory->type != VFS_INODE_DIRECTORY)
		return VFS_ERR_NOTDIR;
	if (node->kind == PROCFS_ROOT)
		return procfs_lookup_root(directory, name, result);
	if (node->kind == PROCFS_PID_DIRECTORY)
		return procfs_lookup_pid(directory, name, result);
	if (node->kind == PROCFS_NET_DIRECTORY)
		return procfs_lookup_net(directory, name, result);
	return VFS_ERR_NOTDIR;
}

static int procfs_readlink(struct vfs_inode *inode, char *buffer,
			   uint32 size)
{
	struct procfs_node *node = inode->private;
	process_t process = cur_proc();

	if (!node || node->kind != PROCFS_SELF || !process)
		return VFS_ERR_INVAL;
	return procfs_format_unsigned(buffer, size, process->pid);
}

static int procfs_getattr(struct vfs_inode *inode, struct vfs_stat *stat)
{
	struct procfs_node *node = inode->private;
	struct process_snapshot snapshot;

	if (node && node->pid > 0 &&
	    process_snapshot_pid(node->pid, &snapshot, 0, 0, 0) == 0) {
		inode->uid = snapshot.euid;
		inode->gid = snapshot.egid;
	}
	return vfs_inode_stat_default(inode, stat);
}

static const struct vfs_inode_operations procfs_inode_operations = {
	.lookup = procfs_lookup,
	.readlink = procfs_readlink,
	.getattr = procfs_getattr,
};

static void procfs_build_pid_stat(struct procfs_buffer *buffer,
				  const struct process_snapshot *snapshot)
{
	uint64 tick_ns = 1000000000ULL / PROCFS_USER_HZ;
	uint64 start = snapshot->start_time_ns / tick_ns;
	uint64 user = snapshot->user_time_ns / tick_ns;
	uint64 system = snapshot->system_time_ns / tick_ns;
	uint64 child_user = snapshot->children_user_time_ns / tick_ns;
	uint64 child_system = snapshot->children_system_time_ns / tick_ns;

	procfs_printf(buffer,
		"%d (%s) %c %d %d %d %d %d 0 0 0 0 0 "
		"%lu %lu %lu %lu %d %d %u 0 %lu %lu %lu 0 0 0 0 0 0 "
		"%lu %lu %lu %lu 0 0 0 17 0 0 0 0 0 0 0 0 0 0 0 0 0 0\n",
		snapshot->pid, snapshot->name, snapshot->state,
		snapshot->ppid, snapshot->pgid, snapshot->sid,
		snapshot->tty, snapshot->tty_pgid, user, system,
		child_user, child_system,
		20 + snapshot->nice, snapshot->nice, snapshot->threads,
		start, snapshot->virtual_size, snapshot->resident_pages,
		snapshot->signal_pending | snapshot->signal_shared_pending,
		snapshot->signal_blocked,
		snapshot->signal_ignored, snapshot->signal_caught);
}

static void procfs_build_pid_status(struct procfs_buffer *buffer,
				    const struct process_snapshot *snapshot)
{
	const char *state = snapshot->state == 'R' ? "running" :
		snapshot->state == 'S' ? "sleeping" :
		snapshot->state == 'D' ? "disk sleep" :
		snapshot->state == 'T' ? "stopped" : "zombie";

	uint32 index;

	procfs_printf(buffer,
		"Name:\t%s\nState:\t%c (%s)\nTgid:\t%d\nPid:\t%d\n"
		"PPid:\t%d\nTracerPid:\t0\n"
		"Uid:\t%u\t%u\t%u\t%u\n"
		"Gid:\t%u\t%u\t%u\t%u\n"
		"FDSize:\t%d\nGroups:\t",
		snapshot->name, snapshot->state, state, snapshot->pid,
		snapshot->pid, snapshot->ppid,
		snapshot->uid, snapshot->euid, snapshot->suid, snapshot->fsuid,
		snapshot->gid, snapshot->egid, snapshot->sgid, snapshot->fsgid,
		NOFILE);
	for (index = 0; index < snapshot->group_count; index++)
		procfs_printf(buffer, "%u ", snapshot->groups[index]);
	procfs_printf(buffer,
		"\nThreads:\t%u\nVmSize:\t%lu kB\nVmRSS:\t%lu kB\n"
		"SigPnd:\t%016lx\nShdPnd:\t%016lx\n"
		"SigBlk:\t%016lx\nSigIgn:\t%016lx\n"
		"SigCgt:\t%016lx\n",
		snapshot->threads, snapshot->virtual_size / 1024,
		snapshot->resident_pages * (PGSIZE / 1024),
		snapshot->signal_pending, snapshot->signal_shared_pending,
		snapshot->signal_blocked, snapshot->signal_ignored,
		snapshot->signal_caught);
}

static void procfs_emit_mount_field(struct procfs_buffer *buffer,
				    const char *field)
{
	while (*field) {
		switch (*field) {
		case ' ':
			procfs_printf(buffer, "\\040");
			break;
		case '\t':
			procfs_printf(buffer, "\\011");
			break;
		case '\n':
			procfs_printf(buffer, "\\012");
			break;
		case '\\':
			procfs_printf(buffer, "\\134");
			break;
		default:
			procfs_emit(*field, buffer);
			break;
		}
		field++;
	}
}

static void procfs_build_mounts(struct procfs_buffer *buffer)
{
	struct vfs_mount_snapshot mounts[16];
	uint32 count, index;

	count = vfs_snapshot_mounts(mounts, NELEM(mounts));
	for (index = 0; index < count; index++) {
		procfs_emit_mount_field(buffer, mounts[index].source);
		procfs_printf(buffer, " ");
		procfs_emit_mount_field(buffer, mounts[index].target);
		procfs_printf(buffer, " %s rw",
			      mounts[index].filesystem);
		if (mounts[index].flags & VFS_MOUNT_NOATIME)
			procfs_printf(buffer, ",noatime");
		if (mounts[index].flags & VFS_MOUNT_NODIRATIME)
			procfs_printf(buffer, ",nodiratime");
		if (mounts[index].flags & VFS_MOUNT_RELATIME)
			procfs_printf(buffer, ",relatime");
		if (mounts[index].flags & VFS_MOUNT_STRICTATIME)
			procfs_printf(buffer, ",strictatime");
		procfs_printf(buffer, " 0 0\n");
	}
}

static void procfs_build_meminfo(struct procfs_buffer *buffer)
{
	uint64 total = palloc_usable_bytes() / 1024;
	uint64 free = palloc_free_pages() * (PGSIZE / 1024);

	procfs_printf(buffer,
		"MemTotal:       %lu kB\n"
		"MemFree:        %lu kB\n"
		"MemAvailable:   %lu kB\n"
		"Buffers:        0 kB\n"
		"Cached:         0 kB\n"
		"SwapCached:     0 kB\n"
		"Active:         0 kB\n"
		"Inactive:       0 kB\n"
		"Shmem:          0 kB\n"
		"SReclaimable:   0 kB\n"
		"SwapTotal:      0 kB\n"
		"SwapFree:       0 kB\n",
		total, free, free);
}

static void procfs_build_uptime(struct procfs_buffer *buffer)
{
	struct process_system_snapshot snapshot;
	uint64 hundredths = ktime_get_boot_ns() / 10000000ULL;
	uint64 idle;

	process_snapshot_system(&snapshot);
	idle = snapshot.idle_time_ns / 10000000ULL;

	procfs_printf(buffer, "%lu.%02lu %lu.%02lu\n",
			hundredths / 100, hundredths % 100,
			idle / 100, idle % 100);
}

static void procfs_build_stat(struct procfs_buffer *buffer)
{
	struct process_system_snapshot snapshot;
	uint64 boot_ns = ktime_get_boot_ns();
	uint64 tick_ns = 1000000000ULL / PROCFS_USER_HZ;
	uint64 realtime_ns = 0, idle_ns, system_ns, total_ns, user_ns;

	process_snapshot_system(&snapshot);
	total_ns = boot_ns * cpu_count();
	idle_ns = snapshot.idle_time_ns < total_ns ?
		snapshot.idle_time_ns : total_ns;
	user_ns = snapshot.user_time_ns < total_ns - idle_ns ?
		snapshot.user_time_ns : total_ns - idle_ns;
	system_ns = total_ns - idle_ns - user_ns;
	procfs_printf(buffer, "cpu  %lu 0 %lu %lu 0 0 0 0 0 0\n",
			user_ns / tick_ns, system_ns / tick_ns,
			idle_ns / tick_ns);
	procfs_printf(buffer, "intr %lu\nctxt %lu\n",
			trap_interrupt_count(), snapshot.context_switches);
	if (ktime_get_realtime_ns(&realtime_ns) == 0 &&
	    realtime_ns >= boot_ns)
		procfs_printf(buffer, "btime %lu\n",
			      (realtime_ns - boot_ns) / 1000000000ULL);
	procfs_printf(buffer, "processes %lu\nprocs_running %u\n"
			      "procs_blocked %u\n",
			snapshot.total_forks, snapshot.running,
			snapshot.blocked);
}

static void procfs_build_loadavg(struct procfs_buffer *buffer)
{
	struct process_system_snapshot snapshot;
	uint32 average[3];

	process_snapshot_system(&snapshot);
	loadavg_get(average);
	procfs_printf(buffer, "%u.%02u %u.%02u %u.%02u %u/%u %d\n",
			average[0] / LOADAVG_FIXED,
			average[0] % LOADAVG_FIXED * 100 / LOADAVG_FIXED,
			average[1] / LOADAVG_FIXED,
			average[1] % LOADAVG_FIXED * 100 / LOADAVG_FIXED,
			average[2] / LOADAVG_FIXED,
			average[2] % LOADAVG_FIXED * 100 / LOADAVG_FIXED,
			snapshot.running, snapshot.processes, snapshot.last_pid);
}

static void procfs_build_net_dev(struct procfs_buffer *buffer)
{
	struct net_device_stats stats;
	struct net_device *device;
	uint32 index;

	procfs_printf(buffer,
		"Inter-|   Receive                                                "
		"|  Transmit\n"
		" face |bytes    packets errs drop fifo frame compressed multicast"
		"|bytes    packets errs drop fifo colls carrier compressed\n");
	for (index = 1; index < NET_DEVICE_MAX; index++) {
		device = net_device_get(index);
		if (!device)
			continue;
		net_device_get_stats(device, &stats);
		procfs_printf(buffer,
			      "%s: %lu %lu 0 %lu 0 0 0 0 %lu %lu 0 %lu 0 0 0 0\n",
			      device->name, stats.rx_bytes, stats.rx_packets,
			      stats.rx_dropped, stats.tx_bytes, stats.tx_packets,
			      stats.tx_dropped);
		net_device_put(device);
	}
}

static int procfs_build_net_route(struct procfs_buffer *buffer)
{
	struct network_interface_snapshot interfaces[NET_DEVICE_MAX];
	uint32 count, index;

	if (network_stack_snapshot_interfaces(interfaces,
		NELEM(interfaces), &count) < 0)
		return VFS_ERR_IO;
	procfs_printf(buffer,
		"Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\t"
		"Mask\t\tMTU\tWindow\tIRTT\n");
	for (index = 0; index < count; index++) {
		struct network_interface_snapshot *interface =
			&interfaces[index];
		uint32 destination;

		if (!interface->up || !interface->ipv4_address ||
		    !interface->ipv4_netmask)
			continue;
		if (!interface->loopback && interface->ipv4_gateway)
			procfs_printf(buffer,
				"%s\t00000000\t%08x\t0003\t0\t0\t0\t"
				"00000000\t0\t0\t0\n",
				interface->name, interface->ipv4_gateway);
		destination = interface->ipv4_address &
			interface->ipv4_netmask;
		procfs_printf(buffer,
			"%s\t%08x\t00000000\t0001\t0\t0\t0\t%08x\t"
			"0\t0\t0\n",
			interface->name, destination,
			interface->ipv4_netmask);
	}
	return VFS_OK;
}

static int procfs_build_net_transport(struct procfs_buffer *buffer,
				      enum procfs_kind kind)
{
	struct ksocket_snapshot *sockets;
	uint32 count, index;
	int type = kind == PROCFS_NET_TCP ?
		LINUX_SOCK_STREAM : LINUX_SOCK_DGRAM;
	uint32 capacity = PROCFS_OPEN_SIZE / sizeof(*sockets);

	sockets = alloc_pages(PROCFS_OPEN_ORDER, PALLOC_ZERO);
	if (!sockets)
		return VFS_ERR_NOMEM;

	procfs_printf(buffer,
		"  sl  local_address rem_address   st tx_queue rx_queue "
		"tr tm->when retrnsmt   uid  timeout inode\n");
	count = ksocket_snapshot_type(type, sockets, capacity);
	for (index = 0; index < count; index++) {
		struct ksocket_snapshot *socket = &sockets[index];
		uint16 local_port = (socket->local.port >> 8) |
			(socket->local.port << 8);
		uint16 remote_port = (socket->remote.port >> 8) |
			(socket->remote.port << 8);

		procfs_printf(buffer,
			"%4u: %08x:%04x %08x:%04x %02x "
			"%08x:%08x 00:00000000 00000000 %u 0 %lu\n",
			index, socket->local.address, local_port,
			socket->remote.address, remote_port, socket->state,
			socket->transmit_queue, socket->receive_queue,
				socket->uid, socket->inode);
	}
	free_pages(sockets, PROCFS_OPEN_ORDER);
	return VFS_OK;
}

static int procfs_file_open(struct vfs_inode *inode, struct vfs_file *file)
{
	struct procfs_node *node = inode->private;
	struct process_snapshot snapshot;
	struct procfs_buffer buffer;
	struct procfs_open *open;
	uint32 cmdline_length = 0;
	int status = VFS_OK;

	if (!node || file->flags & VFS_OPEN_WRITE)
		return VFS_ERR_PERM;
	open = alloc_pages(PROCFS_OPEN_ORDER, PALLOC_ZERO);
	if (!open)
		return VFS_ERR_NOMEM;
	file->private = open;
	buffer.data = open->data;
	buffer.length = 0;
	buffer.capacity = PROCFS_OPEN_SIZE - sizeof(*open);
	buffer.overflow = 0;
	if (node->kind == PROCFS_ROOT) {
		uint32 left, right;

		open->pid_count = process_snapshot_pids(open->pids, NPROC);
		for (right = 1; right < open->pid_count; right++) {
			int value = open->pids[right];

			left = right;
			while (left && open->pids[left - 1] > value) {
				open->pids[left] = open->pids[left - 1];
				left--;
			}
			open->pids[left] = value;
		}
		return VFS_OK;
	}
	if (node->kind == PROCFS_PID_DIRECTORY ||
	    node->kind == PROCFS_NET_DIRECTORY)
		return VFS_OK;
	if (node->kind == PROCFS_PID_CMDLINE) {
		if (process_snapshot_pid(node->pid, &snapshot, buffer.data,
					 buffer.capacity,
					 &cmdline_length) < 0)
			goto no_process;
		open->length = cmdline_length;
		return VFS_OK;
	}
	if (node->kind == PROCFS_PID_STAT ||
	    node->kind == PROCFS_PID_STATUS) {
		if (process_snapshot_pid(node->pid, &snapshot, 0, 0, 0) < 0)
			goto no_process;
		if (node->kind == PROCFS_PID_STAT)
			procfs_build_pid_stat(&buffer, &snapshot);
		else
			procfs_build_pid_status(&buffer, &snapshot);
	} else if (node->kind == PROCFS_MOUNTS) {
		procfs_build_mounts(&buffer);
	} else if (node->kind == PROCFS_MEMINFO) {
		procfs_build_meminfo(&buffer);
	} else if (node->kind == PROCFS_UPTIME) {
		procfs_build_uptime(&buffer);
	} else if (node->kind == PROCFS_STAT) {
		procfs_build_stat(&buffer);
	} else if (node->kind == PROCFS_LOADAVG) {
		procfs_build_loadavg(&buffer);
	} else if (node->kind == PROCFS_NET_DEV) {
		procfs_build_net_dev(&buffer);
	} else if (node->kind == PROCFS_NET_ROUTE) {
		status = procfs_build_net_route(&buffer);
	} else if (node->kind == PROCFS_NET_TCP ||
		   node->kind == PROCFS_NET_UDP) {
		status = procfs_build_net_transport(&buffer, node->kind);
	} else {
		free_pages(open, PROCFS_OPEN_ORDER);
		file->private = 0;
		return VFS_ERR_INVAL;
	}
	if (status < 0) {
		free_pages(open, PROCFS_OPEN_ORDER);
		file->private = 0;
		return status;
	}
	if (buffer.overflow &&
	    (node->kind == PROCFS_NET_TCP || node->kind == PROCFS_NET_UDP)) {
		while (buffer.length && buffer.data[buffer.length - 1] != '\n')
			buffer.length--;
		buffer.overflow = 0;
	}
	if (buffer.overflow) {
		free_pages(open, PROCFS_OPEN_ORDER);
		file->private = 0;
		return VFS_ERR_NOSPC;
	}
	open->length = buffer.length;
	return VFS_OK;

no_process:
	free_pages(open, PROCFS_OPEN_ORDER);
	file->private = 0;
	return VFS_ERR_NOENT;
}

static void procfs_file_release(struct vfs_file *file)
{
	if (file->private)
		free_pages(file->private, PROCFS_OPEN_ORDER);
}

static int64 procfs_file_read(struct vfs_file *file, int user_destination,
			      uint64 destination, uint64 count,
			      uint64 *position)
{
	struct procfs_open *open = file->private;
	uint64 available;

	if (!open || !position)
		return VFS_ERR_INVAL;
	if (*position >= open->length)
		return 0;
	available = open->length - *position;
	if (count > available)
		count = available;
	if (either_copyout(user_destination, destination,
			   open->data + *position, count) < 0)
		return VFS_ERR_FAULT;
	*position += count;
	return count;
}

static int procfs_fill_dirent(struct vfs_file *file,
			      struct vfs_dirent *result,
			      uint64 ino, uint8 type, const char *name)
{
	result->ino = ino;
	result->type = type;
	safe_strncpy(result->name, name, sizeof(result->name));
	result->next_offset = ++file->position;
	return 1;
}

static int procfs_readdir_fixed(struct vfs_file *file,
				struct vfs_dirent *result,
				const char *const *names,
				const uint8 *types,
				const enum procfs_kind *kinds,
				uint32 count, int pid)
{
	uint64 position = file->position;

	if (position >= count)
		return 0;
	return procfs_fill_dirent(file, result,
				   procfs_inode_number(kinds[position], pid),
				   types[position], names[position]);
}

static int procfs_directory_readdir(struct vfs_file *file,
				    struct vfs_dirent *result)
{
	static const char *const root_names[] = {
		".", "..", "self", "mounts", "meminfo", "uptime", "stat",
		"loadavg", "net",
	};
	static const uint8 root_types[] = {
		VFS_DT_DIR, VFS_DT_DIR, VFS_DT_SYMLINK, VFS_DT_REGULAR,
		VFS_DT_REGULAR, VFS_DT_REGULAR, VFS_DT_REGULAR,
		VFS_DT_REGULAR, VFS_DT_DIR,
	};
	static const enum procfs_kind root_kinds[] = {
		PROCFS_ROOT, PROCFS_ROOT, PROCFS_SELF, PROCFS_MOUNTS,
		PROCFS_MEMINFO, PROCFS_UPTIME, PROCFS_STAT,
		PROCFS_LOADAVG, PROCFS_NET_DIRECTORY,
	};
	static const char *const pid_names[] = {
		".", "..", "stat", "status", "cmdline",
	};
	static const uint8 pid_types[] = {
		VFS_DT_DIR, VFS_DT_DIR, VFS_DT_REGULAR, VFS_DT_REGULAR,
		VFS_DT_REGULAR,
	};
	static const enum procfs_kind pid_kinds[] = {
		PROCFS_PID_DIRECTORY, PROCFS_ROOT, PROCFS_PID_STAT,
		PROCFS_PID_STATUS, PROCFS_PID_CMDLINE,
	};
	static const char *const net_names[] = {
		".", "..", "dev", "route", "tcp", "udp",
	};
	static const uint8 net_types[] = {
		VFS_DT_DIR, VFS_DT_DIR, VFS_DT_REGULAR, VFS_DT_REGULAR,
		VFS_DT_REGULAR, VFS_DT_REGULAR,
	};
	static const enum procfs_kind net_kinds[] = {
		PROCFS_NET_DIRECTORY, PROCFS_ROOT, PROCFS_NET_DEV,
		PROCFS_NET_ROUTE, PROCFS_NET_TCP, PROCFS_NET_UDP,
	};
	struct procfs_node *node = file->path.dentry->inode->private;
	struct procfs_open *open = file->private;
	char name[24];
	uint64 position = file->position;
	uint32 fixed_count = NELEM(root_names), index, length;

	if (!node || !open)
		return VFS_ERR_INVAL;
	if (node->kind == PROCFS_PID_DIRECTORY)
		return procfs_readdir_fixed(file, result, pid_names,
					    pid_types, pid_kinds,
					    NELEM(pid_names), node->pid);
	if (node->kind == PROCFS_NET_DIRECTORY)
		return procfs_readdir_fixed(file, result, net_names,
					    net_types, net_kinds,
					    NELEM(net_names), 0);
	if (node->kind != PROCFS_ROOT)
		return VFS_ERR_NOTDIR;
	if (position < fixed_count)
		return procfs_readdir_fixed(file, result, root_names,
					    root_types, root_kinds,
					    fixed_count, 0);
	index = position - fixed_count;
	if (index >= open->pid_count)
		return 0;
	length = procfs_format_unsigned(name, sizeof(name) - 1,
					 open->pids[index]);
	name[length] = 0;
	return procfs_fill_dirent(file, result,
				   procfs_inode_number(PROCFS_PID_DIRECTORY,
						       open->pids[index]),
				   VFS_DT_DIR, name);
}

static const struct vfs_file_operations procfs_file_operations = {
	.flags = VFS_FILE_CAN_PREAD,
	.open = procfs_file_open,
	.release = procfs_file_release,
	.read = procfs_file_read,
};

static const struct vfs_file_operations procfs_directory_operations = {
	.open = procfs_file_open,
	.release = procfs_file_release,
	.readdir = procfs_directory_readdir,
};

static void procfs_put_inode(struct vfs_inode *inode)
{
	free(inode->private);
}

static int procfs_sync(struct vfs_super_block *superblock)
{
	(void)superblock;
	return VFS_OK;
}

static const struct vfs_super_operations procfs_super_operations = {
	.put_inode = procfs_put_inode,
	.sync = procfs_sync,
};

static int procfs_mount(struct vfs_filesystem_type *type,
			struct block_device *device, const void *data,
			struct vfs_super_block **result)
{
	struct vfs_super_block *superblock;

	(void)data;
	if (device)
		return VFS_ERR_INVAL;
	superblock = vfs_super_alloc(type, 0);
	if (!superblock)
		return VFS_ERR_NOMEM;
	superblock->operations = &procfs_super_operations;
	superblock->block_size = PGSIZE;
	superblock->root = procfs_wrap(superblock, PROCFS_ROOT, 0);
	if (!superblock->root) {
		vfs_super_free(superblock);
		return VFS_ERR_NOMEM;
	}
	*result = superblock;
	return VFS_OK;
}

static struct vfs_filesystem_type procfs_type = {
	.name = "proc",
	.flags = VFS_FS_NO_DENTRY_CACHE,
	.mount = procfs_mount,
};

void procfs_init(void)
{
	if (vfs_register_filesystem(&procfs_type) != VFS_OK)
		PANIC("register procfs");
}
