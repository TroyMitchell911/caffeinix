#include <debug.h>
#include <futex.h>
#include <ktime.h>
#include <linux_uapi.h>
#include <process.h>
#include <scheduler.h>
#include <syscall.h>
#include <vm.h>
#include <wait.h>

#define FUTEX_SLOT_COUNT 128
#define FUTEX_ROBUST_LIMIT 2048
#define FUTEX_TID_MASK 0x3fffffffU

struct futex_key {
	process_t process;
	uint64 address;
	uint8 shared;
};

struct futex_slot {
	struct futex_key key;
	struct wait_queue wait;
	uint32 waiters;
	uint8 active;
};

struct linux_robust_list_head {
	uint64 next;
	int64 futex_offset;
	uint64 pending;
};

_Static_assert(sizeof(struct linux_robust_list_head) == 24,
	       "Linux robust-list layout changed");

static struct {
	struct spinlock lock;
	struct futex_slot slots[FUTEX_SLOT_COUNT];
} futex_table;

static int futex_key_equal(const struct futex_key *left,
			   const struct futex_key *right)
{
	return left->process == right->process &&
	       left->address == right->address &&
	       left->shared == right->shared;
}

static int futex_key_get(process_t process, uint64 address, int private,
			 struct futex_key *key)
{
	uint32 value;
	uint64 physical;

	if (!key || address & (sizeof(value) - 1))
		return -LINUX_EINVAL;
	if (copyin(process->pagetable, (char *)&value, address,
	           sizeof(value)) < 0)
		return -LINUX_EFAULT;
	key->shared = !private;
	if (private) {
		key->process = process;
		key->address = address;
		return 0;
	}
	physical = vm_user_pa(process->pagetable, address);
	if (!physical)
		return -LINUX_EFAULT;
	key->process = 0;
	key->address = physical;
	return 0;
}

static struct futex_slot *futex_slot_find_locked(
	const struct futex_key *key)
{
	int index;

	if (!spinlock_holding(&futex_table.lock))
		PANIC("futex lookup unlocked");
	for (index = 0; index < FUTEX_SLOT_COUNT; index++) {
		struct futex_slot *slot = &futex_table.slots[index];

		if (slot->active && futex_key_equal(&slot->key, key))
			return slot;
	}
	return 0;
}

static struct futex_slot *futex_slot_get_locked(
	const struct futex_key *key)
{
	struct futex_slot *slot;
	int index;

	slot = futex_slot_find_locked(key);
	if (slot)
		return slot;
	for (index = 0; index < FUTEX_SLOT_COUNT; index++) {
		slot = &futex_table.slots[index];
		if (slot->active)
			continue;
		slot->key = *key;
		slot->waiters = 0;
		slot->active = 1;
		return slot;
	}
	return 0;
}

static void futex_slot_put_locked(struct futex_slot *slot)
{
	if (!spinlock_holding(&futex_table.lock) || !slot ||
	    !slot->active || !slot->waiters)
		PANIC("invalid futex slot put");
	slot->waiters--;
	if (!slot->waiters) {
		if (!wait_queue_empty(&slot->wait))
			PANIC("empty futex reference count");
		slot->active = 0;
	}
}

static int futex_relative_timeout(process_t process, uint64 address,
				  uint64 *milliseconds)
{
	struct linux_timespec timeout;
	uint64 result;

	if (!address)
		return 0;
	if (copyin(process->pagetable, (char *)&timeout, address,
	           sizeof(timeout)) < 0)
		return -LINUX_EFAULT;
	if (timeout.seconds < 0 || timeout.nanoseconds < 0 ||
	    timeout.nanoseconds >= (int64)NSEC_PER_SEC)
		return -LINUX_EINVAL;
	if (!timeout.seconds && !timeout.nanoseconds)
		return -LINUX_ETIMEDOUT;
	if ((uint64)timeout.seconds >
	    (~(uint64)0 - 999999ULL) / 1000ULL)
		result = ~(uint64)0;
	else
		result = (uint64)timeout.seconds * 1000ULL +
			 ((uint64)timeout.nanoseconds + 999999ULL) /
			 1000000ULL;
	*milliseconds = result;
	return 1;
}

static int futex_absolute_timeout(process_t process, uint64 address,
				  uint64 *milliseconds)
{
	struct linux_timespec timeout;
	uint64 absolute, now;

	if (!address)
		return 0;
	if (copyin(process->pagetable, (char *)&timeout, address,
	           sizeof(timeout)) < 0)
		return -LINUX_EFAULT;
	if (timeout.seconds < 0 || timeout.nanoseconds < 0 ||
	    timeout.nanoseconds >= (int64)NSEC_PER_SEC)
		return -LINUX_EINVAL;
	if ((uint64)timeout.seconds >
	    (~(uint64)0 - (uint64)timeout.nanoseconds) /
	    NSEC_PER_SEC)
		absolute = ~(uint64)0;
	else
		absolute = (uint64)timeout.seconds * NSEC_PER_SEC +
			   (uint64)timeout.nanoseconds;
	now = ktime_get_ns();
	if (absolute <= now)
		return -LINUX_ETIMEDOUT;
	*milliseconds = (absolute - now + 999999ULL) / 1000000ULL;
	return 1;
}

static int futex_wait(uint64 address, int private, uint32 expected,
		      uint64 timeout_address, uint32 bitset,
		      int absolute)
{
	struct futex_key key;
	struct futex_slot *slot;
	process_t process = cur_proc();
	thread_t current = cur_thread();
	uint64 milliseconds = 0;
	uint32 value;
	int result, timed;

	if (!bitset)
		return -LINUX_EINVAL;
	result = futex_key_get(process, address, private, &key);
	if (result < 0)
		return result;
	timed = absolute ?
		futex_absolute_timeout(process, timeout_address,
		                       &milliseconds) :
		futex_relative_timeout(process, timeout_address,
		                       &milliseconds);
	if (timed < 0)
		return timed;

	spinlock_acquire(&futex_table.lock);
	if (copyin(process->pagetable, (char *)&value, address,
	           sizeof(value)) < 0) {
		spinlock_release(&futex_table.lock);
		return -LINUX_EFAULT;
	}
	if (value != expected) {
		spinlock_release(&futex_table.lock);
		return -LINUX_EAGAIN;
	}
	slot = futex_slot_get_locked(&key);
	if (!slot) {
		spinlock_release(&futex_table.lock);
		return -LINUX_ENOMEM;
	}
	slot->waiters++;
	current->wait_private = slot;
	current->wait_bitset = bitset;
	if (timed)
		result = wait_queue_sleep_timeout(&slot->wait,
			&futex_table.lock, milliseconds);
	else {
		wait_queue_sleep(&slot->wait, &futex_table.lock);
		result = 0;
	}
	slot = current->wait_private;
	if (!slot)
		PANIC("futex waiter lost slot");
	futex_slot_put_locked(slot);
	current->wait_private = 0;
	current->wait_bitset = ~(uint32)0;
	spinlock_release(&futex_table.lock);
	return result < 0 ? -LINUX_ETIMEDOUT : 0;
}

static int futex_wake_key_locked(const struct futex_key *key, int count,
				 uint32 bitset)
{
	struct futex_slot *slot;

	slot = futex_slot_find_locked(key);
	if (!slot)
		return 0;
	return wait_queue_wake_mask(&slot->wait, count, bitset);
}

static int futex_wake(uint64 address, int private, int count,
		      uint32 bitset)
{
	struct futex_key key;
	process_t process = cur_proc();
	int result;

	if (count < 0 || !bitset)
		return -LINUX_EINVAL;
	result = futex_key_get(process, address, private, &key);
	if (result < 0)
		return result;
	spinlock_acquire(&futex_table.lock);
	result = futex_wake_key_locked(&key, count, bitset);
	spinlock_release(&futex_table.lock);
	return result;
}

static int futex_requeue(uint64 address, int private, int wake_count,
			 int requeue_count, uint64 destination,
			 int compare, uint32 expected)
{
	struct futex_key source_key, destination_key;
	struct futex_slot *source, *target;
	process_t process = cur_proc();
	uint32 value;
	int moved, result, woken;

	if (wake_count < 0 || requeue_count < 0)
		return -LINUX_EINVAL;
	result = futex_key_get(process, address, private, &source_key);
	if (result < 0)
		return result;
	result = futex_key_get(process, destination, private,
	                       &destination_key);
	if (result < 0)
		return result;
	if (futex_key_equal(&source_key, &destination_key))
		return -LINUX_EINVAL;
	spinlock_acquire(&futex_table.lock);
	if (compare) {
		if (copyin(process->pagetable, (char *)&value, address,
		           sizeof(value)) < 0) {
			spinlock_release(&futex_table.lock);
			return -LINUX_EFAULT;
		}
		if (value != expected) {
			spinlock_release(&futex_table.lock);
			return -LINUX_EAGAIN;
		}
	}
	source = futex_slot_find_locked(&source_key);
	if (!source) {
		spinlock_release(&futex_table.lock);
		return 0;
	}
	woken = wait_queue_wake_mask(&source->wait, wake_count,
	                              LINUX_FUTEX_BITSET_MATCH_ANY);
	if (!requeue_count) {
		spinlock_release(&futex_table.lock);
		return woken;
	}
	target = futex_slot_get_locked(&destination_key);
	if (!target) {
		spinlock_release(&futex_table.lock);
		return woken ? woken : -LINUX_ENOMEM;
	}
	moved = wait_queue_requeue(&source->wait, &target->wait,
	                          requeue_count, target);
	if (source->waiters < (uint32)moved)
		PANIC("futex requeue source count");
	source->waiters -= moved;
	target->waiters += moved;
	if (!source->waiters)
		source->active = 0;
	if (!moved && !target->waiters)
		target->active = 0;
	spinlock_release(&futex_table.lock);
	return woken + moved;
}

uint64 sys_linux_futex(void)
{
	uint64 address, destination, timeout_address;
	int command, count, operation, private, value3;

	argaddr(0, &address);
	argint(1, &operation);
	argint(2, &count);
	argaddr(3, &timeout_address);
	argaddr(4, &destination);
	argint(5, &value3);
	command = operation & LINUX_FUTEX_CMD_MASK;
	private = operation & LINUX_FUTEX_PRIVATE_FLAG;
	if (operation & ~(LINUX_FUTEX_CMD_MASK |
	                  LINUX_FUTEX_PRIVATE_FLAG |
	                  LINUX_FUTEX_CLOCK_REALTIME))
		return -LINUX_EINVAL;
	if ((operation & LINUX_FUTEX_CLOCK_REALTIME) &&
	    command != LINUX_FUTEX_WAIT_BITSET)
		return -LINUX_EINVAL;
	if (operation & LINUX_FUTEX_CLOCK_REALTIME)
		return -LINUX_ENOSYS;
	switch (command) {
	case LINUX_FUTEX_WAIT:
		return futex_wait(address, private, count, timeout_address,
		                  LINUX_FUTEX_BITSET_MATCH_ANY, 0);
	case LINUX_FUTEX_WAIT_BITSET:
		return futex_wait(address, private, count, timeout_address,
		                  (uint32)value3, 1);
	case LINUX_FUTEX_WAKE:
		return futex_wake(address, private, count,
		                  LINUX_FUTEX_BITSET_MATCH_ANY);
	case LINUX_FUTEX_WAKE_BITSET:
		return futex_wake(address, private, count, (uint32)value3);
	case LINUX_FUTEX_REQUEUE:
		return futex_requeue(address, private, count,
		                     (int)timeout_address, destination, 0, 0);
	case LINUX_FUTEX_CMP_REQUEUE:
		return futex_requeue(address, private, count,
		                     (int)timeout_address, destination, 1,
		                     (uint32)value3);
	default:
		return -LINUX_ENOSYS;
	}
}

uint64 sys_linux_set_robust_list(void)
{
	thread_t current = cur_thread();
	uint64 address, length;

	argaddr(0, &address);
	argaddr(1, &length);
	if (length != sizeof(struct linux_robust_list_head))
		return -LINUX_EINVAL;
	current->robust_list = address;
	current->robust_list_len = length;
	return 0;
}

uint64 sys_linux_get_robust_list(void)
{
	process_t process = cur_proc();
	uint64 head_address, length_address;
	uint64 head, length;
	int tid;

	argint(0, &tid);
	argaddr(1, &head_address);
	argaddr(2, &length_address);
	if (!tid)
		tid = cur_thread()->tid;
	if (thread_get_robust_list(tid, &head, &length) < 0)
		return -LINUX_ESRCH;
	if (copyout(process->pagetable, head_address, (char *)&head,
	            sizeof(head)) < 0 ||
	    copyout(process->pagetable, length_address, (char *)&length,
	            sizeof(length)) < 0)
		return -LINUX_EFAULT;
	return 0;
}

static void futex_wake_address(process_t process, uint64 address)
{
	struct futex_key key;

	spinlock_acquire(&futex_table.lock);
	if (!futex_key_get(process, address, 1, &key))
		(void)futex_wake_key_locked(&key, 1,
		                            LINUX_FUTEX_BITSET_MATCH_ANY);
	if (!futex_key_get(process, address, 0, &key))
		(void)futex_wake_key_locked(&key, 1,
		                            LINUX_FUTEX_BITSET_MATCH_ANY);
	spinlock_release(&futex_table.lock);
}

static void futex_robust_mark(process_t process, uint64 node,
			      int64 offset, int tid)
{
	volatile uint32 *word;
	pte_t *pte;
	uint64 address, physical;
	uint64 delta;
	uint32 old, replacement;

	node &= ~1ULL;
	if (!node)
		return;
	if (offset < 0) {
		delta = 0 - (uint64)offset;
		if (node < delta)
			return;
		address = node - delta;
	} else {
		if (node > ~(uint64)0 - (uint64)offset)
			return;
		address = node + (uint64)offset;
	}
	if (address & (sizeof(*word) - 1))
		return;
	pte = PTE(process->pagetable, address, 0);
	if (!pte || !(*pte & PTE_V) || !(*pte & PTE_U) ||
	    !(*pte & PTE_W))
		return;
	physical = vm_user_pa(process->pagetable, address);
	if (!physical)
		return;
	word = (volatile uint32 *)physical;
	old = __atomic_load_n(word, __ATOMIC_ACQUIRE);
	while ((old & FUTEX_TID_MASK) == (uint32)tid) {
		replacement = (old & LINUX_FUTEX_WAITERS) |
			      LINUX_FUTEX_OWNER_DIED;
		if (__atomic_compare_exchange_n(word, &old, replacement, 0,
		                                __ATOMIC_RELEASE,
		                                __ATOMIC_ACQUIRE)) {
			futex_wake_address(process, address);
			break;
		}
	}
}

void futex_thread_exit(thread_t thread)
{
	struct linux_robust_list_head head;
	process_t process = thread->home;
	uint64 current, next, pending = 0;
	uint32 zero = 0;
	int count;

	if (thread->robust_list &&
	    thread->robust_list_len == sizeof(head) &&
	    copyin(process->pagetable, (char *)&head,
	           thread->robust_list, sizeof(head)) == 0) {
		pending = head.pending;
		current = head.next;
		for (count = 0; count < FUTEX_ROBUST_LIMIT &&
		     (current & ~1ULL) != thread->robust_list; count++) {
			if (!current || copyin(process->pagetable, (char *)&next,
			                       current & ~1ULL,
			                       sizeof(next)) < 0)
				break;
			futex_robust_mark(process, current, head.futex_offset,
			                  thread->tid);
			if ((current & ~1ULL) == (pending & ~1ULL))
				pending = 0;
			current = next;
		}
		if (pending)
			futex_robust_mark(process, pending, head.futex_offset,
			                  thread->tid);
	}
	if (thread->clear_child_tid &&
	    copyout(process->pagetable, thread->clear_child_tid,
	            (char *)&zero, sizeof(zero)) == 0)
		futex_wake_address(process, thread->clear_child_tid);
}

void futex_init(void)
{
	int index;

	spinlock_init(&futex_table.lock, "futex table");
	for (index = 0; index < FUTEX_SLOT_COUNT; index++) {
		struct futex_slot *slot = &futex_table.slots[index];

		slot->active = 0;
		slot->waiters = 0;
		wait_queue_init(&slot->wait, "futex");
	}
}
