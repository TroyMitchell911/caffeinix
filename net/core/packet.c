#include <debug.h>
#include <mystring.h>
#include <netdevice.h>
#include <palloc.h>

static struct {
	struct spinlock lock;
	struct net_packet packets[NET_PACKET_POOL_SIZE];
	struct net_packet *free;
	uint32 available;
} packet_pool;

void net_packet_pool_init(void)
{
	uint32 index;

	spinlock_init(&packet_pool.lock, "network packet pool");
	for (index = 0; index < NET_PACKET_POOL_SIZE; index++) {
		struct net_packet *packet = &packet_pool.packets[index];

		packet->buffer = palloc();
		if (!packet->buffer)
			break;
		spinlock_init(&packet->lock, "network packet");
		packet->next_free = packet_pool.free;
		packet_pool.free = packet;
		packet_pool.available++;
	}
	if (!packet_pool.available)
		PANIC("network packet pool");
}

struct net_packet *net_packet_alloc(uint32 capacity)
{
	struct net_packet *packet;

	if (!capacity || capacity > NET_PACKET_SIZE)
		return 0;
	spinlock_acquire(&packet_pool.lock);
	packet = packet_pool.free;
	if (packet) {
		packet_pool.free = packet->next_free;
		packet_pool.available--;
	}
	spinlock_release(&packet_pool.lock);
	if (!packet)
		return 0;
	packet->device = 0;
	packet->data = packet->buffer + NET_PACKET_HEADROOM;
	packet->capacity = capacity;
	packet->length = 0;
	packet->refcount = 1;
	packet->next_free = 0;
	packet->next_receive = 0;
	packet->pooled = 1;
	return packet;
}

struct net_packet *net_packet_get(struct net_packet *packet)
{
	if (!packet)
		return 0;
	spinlock_acquire(&packet->lock);
	if (!packet->pooled || !packet->refcount ||
	    packet->refcount == ~(uint32)0) {
		spinlock_release(&packet->lock);
		return 0;
	}
	packet->refcount++;
	spinlock_release(&packet->lock);
	return packet;
}

void net_packet_put(struct net_packet *packet)
{
	int release = 0;

	if (!packet)
		return;
	spinlock_acquire(&packet->lock);
	if (!packet->pooled || !packet->refcount)
		PANIC("network packet underflow");
	if (!--packet->refcount) {
		packet->pooled = 0;
		release = 1;
	}
	spinlock_release(&packet->lock);
	if (!release)
		return;
	spinlock_acquire(&packet_pool.lock);
	packet->next_receive = 0;
	packet->next_free = packet_pool.free;
	packet_pool.free = packet;
	packet_pool.available++;
	if (packet_pool.available > NET_PACKET_POOL_SIZE)
		PANIC("network packet pool overflow");
	spinlock_release(&packet_pool.lock);
}

uint32 net_packet_pool_available(void)
{
	uint32 available;

	spinlock_acquire(&packet_pool.lock);
	available = packet_pool.available;
	spinlock_release(&packet_pool.lock);
	return available;
}
