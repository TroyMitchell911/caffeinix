#include <debug.h>
#include <kernel_config.h>
#include <mystring.h>
#include <netdevice.h>
#include <scheduler.h>
#include <thread.h>
#include <wait.h>
#include <workqueue.h>

struct net_callback_slot {
	int thread;
	int cpu;
};

static struct {
	struct spinlock lock;
	struct net_device *devices[NET_DEVICE_MAX];
	net_receive_t receive;
	void *receive_argument;
	uint32 receive_active;
	uint32 receive_threads[NTHREAD];
	uint32 receive_cpus[NCPU];
	uint8 receive_draining;
	uint8 receive_dispatching;
	struct net_packet *receive_head;
	struct net_packet *receive_tail;
	struct wait_queue receive_wait;
	net_state_t state;
	void *state_argument;
	uint32 state_active;
	uint32 state_threads[NTHREAD];
	uint32 state_cpus[NCPU];
	uint8 state_draining;
	struct net_device *state_head;
	struct net_device *state_tail;
	struct wait_queue state_wait;
	struct work_struct state_work;
} network;

extern void net_packet_pool_init(void);
static void net_state_notify_pending(struct net_device *device);

static int net_state_queue_locked(struct net_device *device)
{
	int queued = 0;

	spinlock_acquire(&device->lock);
	if (device->state_pending && !device->state_queued) {
		device->state_queued = 1;
		device->next_state = 0;
		if (network.state_tail)
			network.state_tail->next_state = device;
		else
			network.state_head = device;
		network.state_tail = device;
		queued = 1;
	}
	spinlock_release(&device->lock);
	return queued;
}

static struct net_device *net_state_next_queued(void)
{
	struct net_device *device;

	spinlock_acquire(&network.lock);
	device = network.state_head;
	if (device) {
		network.state_head = device->next_state;
		if (!network.state_head)
			network.state_tail = 0;
		spinlock_acquire(&device->lock);
		device->next_state = 0;
		device->state_queued = 0;
		spinlock_release(&device->lock);
	}
	spinlock_release(&network.lock);
	return device;
}

static void net_state_work(struct work_struct *work)
{
	struct net_device *device;

	(void)work;
	while ((device = net_state_next_queued()))
		net_state_notify_pending(device);
}

static struct net_callback_slot net_callback_slot(void)
{
	struct net_callback_slot slot = { .thread = -1, .cpu = -1 };
	thread_t current = cur_thread();

	if (current) {
		slot.thread = current - thread;
		if (slot.thread < 0 || slot.thread >= NTHREAD)
			slot.thread = -1;
	} else if (cpuid() < NCPU) {
		slot.cpu = cpuid();
	}
	return slot;
}

static uint32 net_callback_owned_locked(uint32 *threads, uint32 *cpus)
{
	struct net_callback_slot slot = net_callback_slot();

	if (slot.thread >= 0)
		return threads[slot.thread];
	if (slot.cpu >= 0)
		return cpus[slot.cpu];
	return 0;
}

static void net_callback_wait_turn_locked(uint32 *active, uint32 *threads,
					  uint32 *cpus,
					  struct wait_queue *wait)
{
	while (*active && !net_callback_owned_locked(threads, cpus))
		wait_queue_sleep(wait, &network.lock);
}

static uint32 net_device_transmit_owned_locked(struct net_device *device)
{
	return net_callback_owned_locked(device->transmit_threads,
					  device->transmit_cpus);
}

static void net_callback_enter_locked(struct net_callback_slot *slot,
				      uint32 *threads, uint32 *cpus)
{
	*slot = net_callback_slot();
	if (slot->thread >= 0)
		threads[slot->thread]++;
	else if (slot->cpu >= 0)
		cpus[slot->cpu]++;
}

static void net_callback_leave_locked(struct net_callback_slot *slot,
				      uint32 *threads, uint32 *cpus)
{
	if (slot->thread >= 0)
		threads[slot->thread]--;
	else if (slot->cpu >= 0)
		cpus[slot->cpu]--;
}

static int net_device_name_exists_locked(const char *name)
{
	uint32 index;

	for (index = 1; index < NET_DEVICE_MAX; index++) {
		if (network.devices[index] &&
		    !strcmp(network.devices[index]->name, name))
			return 1;
	}
	return 0;
}

static int net_device_assign_name_locked(struct net_device *device)
{
	uint32 number;

	if (device->name[0])
		return net_device_name_exists_locked(device->name) ? -1 : 0;
	for (number = 0; number < NET_DEVICE_MAX - 1; number++) {
		safe_strncpy(device->name, "eth0", sizeof(device->name));
		device->name[3] = '0' + number;
		if (!net_device_name_exists_locked(device->name))
			return 0;
	}
	device->name[0] = 0;
	return -1;
}

void net_device_init(void)
{
	spinlock_init(&network.lock, "network devices");
	wait_queue_init(&network.receive_wait, "network receive callbacks");
	wait_queue_init(&network.state_wait, "network state callbacks");
	work_init(&network.state_work, net_state_work);
	net_packet_pool_init();
}

int net_device_register(struct net_device *device)
{
	uint32 free_index = 0, index;

	if (!device || !device->operations ||
	    !device->operations->start_xmit || !device->mtu ||
	    device->mtu > NET_PACKET_SIZE - NET_ETH_HEADER_LENGTH)
		return -1;
	spinlock_acquire(&network.lock);
	if (device->registered) {
		spinlock_release(&network.lock);
		return -1;
	}
	if (net_device_assign_name_locked(device) < 0) {
		spinlock_release(&network.lock);
		return -1;
	}
	for (index = 1; index < NET_DEVICE_MAX; index++) {
		if (!network.devices[index] && !free_index)
			free_index = index;
	}
	if (!free_index) {
		spinlock_release(&network.lock);
		return -1;
	}
	index = free_index;
	device->index = index;
	device->references = 0;
	device->registered = 1;
	device->up = 0;
	device->carrier = 0;
	device->queue_stopped = 0;
	device->transmit_active = 0;
	memset(device->transmit_threads, 0,
	       sizeof(device->transmit_threads));
	memset(device->transmit_cpus, 0, sizeof(device->transmit_cpus));
	device->lifecycle_transition = 0;
	device->stop_pending = 0;
	device->state_pending = 0;
	device->state_queued = 0;
	device->next_state = 0;
	memset(&device->stats, 0, sizeof(device->stats));
	spinlock_init(&device->lock, device->name);
	wait_queue_init(&device->lifecycle_wait, device->name);
	network.devices[index] = device;
	spinlock_release(&network.lock);
	return 0;
}

int net_device_unregister(struct net_device *device)
{
	int was_up;

	if (!device)
		return -1;
	for (;;) {
		spinlock_acquire(&network.lock);
		if (!device->registered || !device->index ||
		    device->index >= NET_DEVICE_MAX ||
		    network.devices[device->index] != device) {
			spinlock_release(&network.lock);
			return -1;
		}
		if (net_callback_owned_locked(network.state_threads,
					      network.state_cpus)) {
			spinlock_release(&network.lock);
			return -1;
		}
		spinlock_acquire(&device->lock);
		if (net_device_transmit_owned_locked(device)) {
			spinlock_release(&device->lock);
			spinlock_release(&network.lock);
			return -1;
		}
		if (device->references) {
			if (net_callback_owned_locked(
			    network.receive_threads,
			    network.receive_cpus)) {
				spinlock_release(&device->lock);
				spinlock_release(&network.lock);
				return -1;
			}
			spinlock_release(&network.lock);
			wait_queue_sleep(&device->lifecycle_wait,
					 &device->lock);
			spinlock_release(&device->lock);
			continue;
		}
		if (device->lifecycle_transition || device->state_pending) {
			spinlock_release(&network.lock);
			wait_queue_sleep(&device->lifecycle_wait,
					 &device->lock);
			spinlock_release(&device->lock);
			continue;
		}
		network.devices[device->index] = 0;
		device->registered = 0;
		device->index = 0;
		device->lifecycle_transition = 1;
		was_up = device->up;
		device->up = 0;
		device->queue_stopped = 1;
		spinlock_release(&device->lock);
		spinlock_release(&network.lock);
		break;
	}

	spinlock_acquire(&device->lock);
	while (device->transmit_active)
		wait_queue_sleep(&device->lifecycle_wait, &device->lock);
	spinlock_release(&device->lock);
	if (was_up && device->operations->stop)
		device->operations->stop(device);
	spinlock_acquire(&device->lock);
	device->state_pending++;
	device->lifecycle_transition = 0;
	wait_queue_wake_all(&device->lifecycle_wait);
	spinlock_release(&device->lock);
	net_state_notify_pending(device);
	return 0;
}

struct net_device *net_device_get(uint32 index)
{
	struct net_device *device;

	if (!index || index >= NET_DEVICE_MAX)
		return 0;
	spinlock_acquire(&network.lock);
	device = network.devices[index];
	if (device) {
		spinlock_acquire(&device->lock);
		device->references++;
		spinlock_release(&device->lock);
	}
	spinlock_release(&network.lock);
	return device;
}

struct net_device *net_device_first(void)
{
	uint32 index;

	spinlock_acquire(&network.lock);
	for (index = 1; index < NET_DEVICE_MAX; index++) {
		if (network.devices[index]) {
			struct net_device *device = network.devices[index];

			spinlock_acquire(&device->lock);
			device->references++;
			spinlock_release(&device->lock);
			spinlock_release(&network.lock);
			return device;
		}
	}
	spinlock_release(&network.lock);
	return 0;
}

void net_device_put(struct net_device *device)
{
	if (!device)
		return;
	spinlock_acquire(&device->lock);
	if (!device->references)
		PANIC("put unreferenced network device");
	device->references--;
	if (!device->references)
		wait_queue_wake_all(&device->lifecycle_wait);
	spinlock_release(&device->lock);
}

int net_device_open(struct net_device *device)
{
	int status;

	if (!device)
		return -1;
	spinlock_acquire(&network.lock);
	if (!device->registered || !device->index ||
	    device->index >= NET_DEVICE_MAX ||
	    network.devices[device->index] != device) {
		spinlock_release(&network.lock);
		return -1;
	}
	spinlock_acquire(&device->lock);
	spinlock_release(&network.lock);
	while (device->lifecycle_transition) {
		if (net_device_transmit_owned_locked(device)) {
			spinlock_release(&device->lock);
			return -1;
		}
		wait_queue_sleep(&device->lifecycle_wait, &device->lock);
	}
	if (!device->registered) {
		spinlock_release(&device->lock);
		return -1;
	}
	if (device->up) {
		spinlock_release(&device->lock);
		return 0;
	}
	device->lifecycle_transition = 1;
	spinlock_release(&device->lock);
	status = device->operations->open ?
		device->operations->open(device) : 0;
	spinlock_acquire(&device->lock);
	if (!status) {
		device->up = 1;
		device->queue_stopped = 0;
		device->state_pending++;
	}
	device->lifecycle_transition = 0;
	wait_queue_wake_all(&device->lifecycle_wait);
	spinlock_release(&device->lock);
	if (!status)
		net_state_notify_pending(device);
	return status;
}

static void net_device_finish_close(struct net_device *device)
{
	if (device->operations->stop)
		device->operations->stop(device);
	spinlock_acquire(&device->lock);
	device->stop_pending = 0;
	device->state_pending++;
	device->lifecycle_transition = 0;
	wait_queue_wake_all(&device->lifecycle_wait);
	spinlock_release(&device->lock);
	net_state_notify_pending(device);
}

void net_device_close(struct net_device *device)
{
	uint32 owned;

	if (!device)
		return;
	spinlock_acquire(&device->lock);
	while (device->lifecycle_transition) {
		if (net_device_transmit_owned_locked(device)) {
			spinlock_release(&device->lock);
			return;
		}
		wait_queue_sleep(&device->lifecycle_wait, &device->lock);
	}
	if (!device->up) {
		spinlock_release(&device->lock);
		return;
	}
	device->lifecycle_transition = 1;
	device->up = 0;
	device->queue_stopped = 1;
	owned = net_device_transmit_owned_locked(device);
	while (device->transmit_active > owned)
		wait_queue_sleep(&device->lifecycle_wait, &device->lock);
	if (device->transmit_active) {
		device->stop_pending = 1;
		spinlock_release(&device->lock);
		return;
	}
	spinlock_release(&device->lock);
	net_device_finish_close(device);
}

static void net_state_notify_pending(struct net_device *device)
{
	net_state_t state;
	void *argument;
	struct net_callback_slot slot;
	struct net_device *queued;
	int pending;

	spinlock_acquire(&network.lock);
	if (network.state_active && net_callback_owned_locked(
	    network.state_threads, network.state_cpus)) {
		pending = net_state_queue_locked(device);
		spinlock_release(&network.lock);
		if (pending)
			schedule_work(&network.state_work);
		return;
	}
	net_callback_wait_turn_locked(&network.state_active,
				      network.state_threads,
				      network.state_cpus,
				      &network.state_wait);
	state = network.state;
	argument = network.state_argument;
	spinlock_acquire(&device->lock);
	pending = !!device->state_pending;
	device->state_pending = 0;
	wait_queue_wake_all(&device->lifecycle_wait);
	spinlock_release(&device->lock);
	if (state && pending) {
		network.state_active++;
		net_callback_enter_locked(&slot, network.state_threads,
					  network.state_cpus);
	}
	spinlock_release(&network.lock);
	if (state && pending)
		state(device, argument);
	spinlock_acquire(&network.lock);
	if (state && pending) {
		network.state_active--;
		net_callback_leave_locked(&slot, network.state_threads,
					  network.state_cpus);
	}
	if (network.state_draining && !network.state_active &&
	    !network.state_head)
		network.state_draining = 0;
	wait_queue_wake_all(&network.state_wait);
	spinlock_release(&network.lock);
	queued = net_state_next_queued();
	if (queued)
		net_state_notify_pending(queued);
}

void net_device_set_carrier(struct net_device *device, int carrier)
{
	int notify = 0, queue = 0;

	if (!device)
		return;
	carrier = !!carrier;
	spinlock_acquire(&device->lock);
	if (device->carrier != carrier && device->registered &&
	    !device->lifecycle_transition) {
		device->state_pending++;
		if (workqueue_in_worker()) {
			queue = 1;
		} else {
			notify = 1;
		}
	}
	device->carrier = carrier;
	spinlock_release(&device->lock);
	/*
	 * A driver may report carrier state from work embedded in its private
	 * allocation.  Run callbacks from core-owned work so a callback can
	 * synchronously unregister and release that driver after its work has
	 * unwound.
	 */
	if (queue) {
		spinlock_acquire(&network.lock);
		queue = net_state_queue_locked(device);
		spinlock_release(&network.lock);
		if (queue)
			schedule_work(&network.state_work);
	}
	else if (notify)
		net_state_notify_pending(device);
}

int net_device_carrier_ok(struct net_device *device)
{
	int carrier;

	if (!device)
		return 0;
	spinlock_acquire(&device->lock);
	carrier = device->carrier;
	spinlock_release(&device->lock);
	return carrier;
}

void netif_stop_queue(struct net_device *device)
{
	if (!device)
		return;
	spinlock_acquire(&device->lock);
	device->queue_stopped = 1;
	spinlock_release(&device->lock);
}

void netif_wake_queue(struct net_device *device)
{
	if (!device)
		return;
	spinlock_acquire(&device->lock);
	if (device->up)
		device->queue_stopped = 0;
	spinlock_release(&device->lock);
}

int netif_queue_stopped(struct net_device *device)
{
	int stopped;

	if (!device)
		return 1;
	spinlock_acquire(&device->lock);
	stopped = device->queue_stopped;
	spinlock_release(&device->lock);
	return stopped;
}

int net_device_xmit(struct net_device *device,
		    struct net_packet *packet)
{
	struct net_callback_slot slot;
	uint32 length;
	int finish_close = 0, status;

	if (!device || !packet ||
	    packet->length < NET_ETH_HEADER_LENGTH ||
	    packet->length > packet->capacity)
		return -1;
	spinlock_acquire(&device->lock);
	if (packet->length > device->mtu + NET_ETH_HEADER_LENGTH) {
		spinlock_release(&device->lock);
		return -1;
	}
	if (!device->registered || !device->up || !device->carrier ||
	    device->queue_stopped) {
		spinlock_release(&device->lock);
		return NETDEV_TX_BUSY;
	}
	device->transmit_active++;
	net_callback_enter_locked(&slot, device->transmit_threads,
				  device->transmit_cpus);
	spinlock_release(&device->lock);
	packet->device = device;
	length = packet->length;
	status = device->operations->start_xmit(device, packet);
	if (status < 0) {
		if (status != NETDEV_TX_BUSY)
			__atomic_fetch_add(&device->stats.tx_dropped, 1,
					   __ATOMIC_RELAXED);
	} else {
		__atomic_fetch_add(&device->stats.tx_packets, 1,
				   __ATOMIC_RELAXED);
		__atomic_fetch_add(&device->stats.tx_bytes, length,
				   __ATOMIC_RELAXED);
		status = 0;
	}
	spinlock_acquire(&device->lock);
	device->transmit_active--;
	net_callback_leave_locked(&slot, device->transmit_threads,
				  device->transmit_cpus);
	if (!device->transmit_active && device->stop_pending) {
		device->stop_pending = 0;
		finish_close = 1;
	}
	if (device->lifecycle_transition)
		wait_queue_wake_all(&device->lifecycle_wait);
	spinlock_release(&device->lock);
	if (finish_close)
		net_device_finish_close(device);
	return status;
}

void net_device_rx_drop(struct net_device *device)
{
	if (device)
		__atomic_fetch_add(&device->stats.rx_dropped, 1,
				   __ATOMIC_RELAXED);
}

void net_device_get_stats(struct net_device *device,
			  struct net_device_stats *stats)
{
	if (!device || !stats)
		return;
	stats->rx_packets = __atomic_load_n(&device->stats.rx_packets,
					    __ATOMIC_RELAXED);
	stats->rx_bytes = __atomic_load_n(&device->stats.rx_bytes,
					  __ATOMIC_RELAXED);
	stats->rx_dropped = __atomic_load_n(&device->stats.rx_dropped,
					    __ATOMIC_RELAXED);
	stats->tx_packets = __atomic_load_n(&device->stats.tx_packets,
					    __ATOMIC_RELAXED);
	stats->tx_bytes = __atomic_load_n(&device->stats.tx_bytes,
					  __ATOMIC_RELAXED);
	stats->tx_dropped = __atomic_load_n(&device->stats.tx_dropped,
					    __ATOMIC_RELAXED);
}

int net_receive_register(net_receive_t receive, void *argument)
{
	if (!receive)
		return -1;
	spinlock_acquire(&network.lock);
	if (network.receive || network.receive_draining) {
		spinlock_release(&network.lock);
		return -1;
	}
	network.receive = receive;
	network.receive_argument = argument;
	spinlock_release(&network.lock);
	return 0;
}

void net_receive_unregister(net_receive_t receive, void *argument)
{
	uint32 owned;

	spinlock_acquire(&network.lock);
	if (network.receive == receive &&
	    network.receive_argument == argument) {
		network.receive = 0;
		network.receive_argument = 0;
		network.receive_draining = 1;
	}
	if (network.receive_draining) {
		owned = net_callback_owned_locked(network.receive_threads,
						 network.receive_cpus);
		while (network.receive_active > owned ||
		       (!owned && network.receive_dispatching))
			wait_queue_sleep(&network.receive_wait,
					 &network.lock);
		if (!network.receive_active && !network.receive_dispatching)
			network.receive_draining = 0;
	}
	spinlock_release(&network.lock);
}

int net_state_register(net_state_t state, void *argument)
{
	if (!state)
		return -1;
	spinlock_acquire(&network.lock);
	if (network.state || network.state_draining) {
		spinlock_release(&network.lock);
		return -1;
	}
	network.state = state;
	network.state_argument = argument;
	spinlock_release(&network.lock);
	return 0;
}

void net_state_unregister(net_state_t state, void *argument)
{
	uint32 owned;

	spinlock_acquire(&network.lock);
	if (network.state == state && network.state_argument == argument) {
		network.state = 0;
		network.state_argument = 0;
		network.state_draining = 1;
	}
	if (network.state_draining) {
		owned = !!net_callback_owned_locked(network.state_threads,
						  network.state_cpus);
		while (network.state_active > owned)
			wait_queue_sleep(&network.state_wait, &network.lock);
		if (!network.state_active && !network.state_head)
			network.state_draining = 0;
	}
	spinlock_release(&network.lock);
}

void netif_receive(struct net_packet *packet)
{
	net_receive_t receive;
	void *argument;
	struct net_callback_slot slot;
	struct net_device *device;

	if (!packet || !packet->device) {
		net_packet_put(packet);
		return;
	}
	device = packet->device;
	spinlock_acquire(&network.lock);
	if (!device->registered || !device->index ||
	    device->index >= NET_DEVICE_MAX ||
	    network.devices[device->index] != device) {
		spinlock_release(&network.lock);
		net_packet_put(packet);
		return;
	}
	spinlock_acquire(&device->lock);
	if (packet->length < NET_ETH_HEADER_LENGTH ||
	    packet->length > packet->capacity ||
	    packet->length > device->mtu + NET_ETH_HEADER_LENGTH) {
		__atomic_fetch_add(&device->stats.rx_dropped, 1,
				   __ATOMIC_RELAXED);
		spinlock_release(&device->lock);
		spinlock_release(&network.lock);
		net_packet_put(packet);
		return;
	}
	device->references++;
	spinlock_release(&device->lock);
	__atomic_fetch_add(&device->stats.rx_packets, 1,
			   __ATOMIC_RELAXED);
	__atomic_fetch_add(&device->stats.rx_bytes, packet->length,
			   __ATOMIC_RELAXED);
	if (!network.receive) {
		spinlock_release(&network.lock);
		net_device_rx_drop(device);
		net_packet_put(packet);
		net_device_put(device);
		return;
	}
	packet->next_receive = 0;
	if (network.receive_tail)
		network.receive_tail->next_receive = packet;
	else
		network.receive_head = packet;
	network.receive_tail = packet;
	if (network.receive_dispatching) {
		spinlock_release(&network.lock);
		return;
	}
	network.receive_dispatching = 1;
	for (;;) {
		packet = network.receive_head;
		network.receive_head = packet->next_receive;
		if (!network.receive_head)
			network.receive_tail = 0;
		packet->next_receive = 0;
		device = packet->device;
		receive = network.receive;
		argument = network.receive_argument;
		if (receive) {
			network.receive_active++;
			net_callback_enter_locked(&slot,
						  network.receive_threads,
						  network.receive_cpus);
		}
		spinlock_release(&network.lock);
		if (receive)
			receive(packet, argument);
		else {
			net_device_rx_drop(device);
			net_packet_put(packet);
		}
		net_device_put(device);
		spinlock_acquire(&network.lock);
		if (receive) {
			network.receive_active--;
			net_callback_leave_locked(&slot,
						  network.receive_threads,
						  network.receive_cpus);
		}
		if (network.receive_head)
			continue;
		network.receive_dispatching = 0;
		if (network.receive_draining && !network.receive_active)
			network.receive_draining = 0;
		wait_queue_wake_all(&network.receive_wait);
		spinlock_release(&network.lock);
		return;
	}
}
