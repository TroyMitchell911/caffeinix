/*
 * Linux futex and robust-list implementation.
 *
 * Waiters are keyed by either a process-local virtual address or a shared
 * physical address. The fixed table is protected by futex_table.lock, which
 * is held while a waiter is checked, attached, woken, or requeued.
 */
#include <debug.h>
#include <futex.h>
#include <ktime.h>
#include <linux_uapi.h>
#include <process.h>
#include <scheduler.h>
#include <signal.h>
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

/* Clear a thread's saved restart state; the caller serializes @thread. */
void futex_restart_cancel(thread_t thread)
{
	if (!thread)
		return;
	thread->futex_restart_active = 0;
	thread->futex_restart_armed = 0;
	thread->futex_restart_resume = 0;
	thread->futex_restart_private = 0;
	thread->futex_restart_epc = 0;
	thread->futex_restart_address = 0;
	thread->futex_restart_timeout_address = 0;
	thread->futex_restart_deadline = 0;
	thread->futex_restart_expected = 0;
	thread->futex_restart_bitset = 0;
}

/* Select direct resume or sigreturn-mediated resume after signal delivery. */
void futex_restart_signal(thread_t thread, int through_handler)
{
	if (!thread || !thread->futex_restart_active)
		return;
	thread->futex_restart_armed = !!through_handler;
	thread->futex_restart_resume = !through_handler;
}

/* Re-arm a saved wait only when sigreturn restored the original syscall. */
void futex_restart_sigreturn(thread_t thread)
{
	if (!thread || !thread->futex_restart_active ||
	    !thread->futex_restart_armed)
		return;
	if (thread->trapframe->epc != thread->futex_restart_epc ||
	    thread->trapframe->a7 != LINUX_SYS_futex ||
	    thread->trapframe->a0 != thread->futex_restart_address) {
		futex_restart_cancel(thread);
		return;
	}
	thread->futex_restart_armed = 0;
	thread->futex_restart_resume = 1;
}

/* Consume matching restart state and return its original absolute deadline. */
static int futex_restart_take(thread_t thread, uint64 address, int private,
			      uint32 expected, uint64 timeout_address,
			      uint32 bitset, uint64 *deadline)
{
	uint64 epc = thread->trapframe->epc - 4;

	if (thread->futex_restart_active && thread->futex_restart_resume &&
	    thread->futex_restart_epc == epc &&
	    thread->futex_restart_address == address &&
	    thread->futex_restart_private == !!private &&
	    thread->futex_restart_expected == expected &&
	    thread->futex_restart_timeout_address == timeout_address &&
	    thread->futex_restart_bitset == bitset) {
		*deadline = thread->futex_restart_deadline;
		thread->futex_restart_resume = 0;
		return 1;
	}
	futex_restart_cancel(thread);
	return 0;
}

/* Save enough syscall state to retain a relative timeout across restart. */
static void futex_restart_set(thread_t thread, uint64 address, int private,
			      uint32 expected, uint64 timeout_address,
			      uint32 bitset, uint64 deadline)
{
	thread->futex_restart_active = 1;
	thread->futex_restart_armed = 0;
	thread->futex_restart_resume = 0;
	thread->futex_restart_private = !!private;
	thread->futex_restart_epc = thread->trapframe->epc - 4;
	thread->futex_restart_address = address;
	thread->futex_restart_timeout_address = timeout_address;
	thread->futex_restart_deadline = deadline;
	thread->futex_restart_expected = expected;
	thread->futex_restart_bitset = bitset;
}

/* Compare fully normalized process-private or physical shared keys. */
static int futex_key_equal(const struct futex_key *left,
			   const struct futex_key *right)
{
	return left->process == right->process &&
	       left->address == right->address &&
	       left->shared == right->shared;
}

/* Resolve a validated user word into a private virtual or shared key. */
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

/* Find an active slot. futex_table.lock is held. */
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

/* Find or allocate a slot. futex_table.lock is held. */
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

/* Drop one waiter reference and deactivate an empty slot under table lock. */
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

/* Convert a userspace relative timespec to rounded-up milliseconds. */
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

/* Convert a userspace absolute monotonic timespec to remaining milliseconds. */
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

/* Check @expected and sleep atomically with futex_table.lock held. */
static int futex_wait(uint64 address, int private, uint32 expected,
		      uint64 timeout_address, uint32 bitset,
		      int absolute)
{
	struct futex_key key;
	struct futex_slot *slot;
	process_t process = cur_proc();
	thread_t current = cur_thread();
	uint64 deadline = 0, milliseconds = 0;
	uint32 value;
	int result, restarted = 0, timed;

	if (!bitset)
		return -LINUX_EINVAL;
	result = futex_key_get(process, address, private, &key);
	if (result < 0)
		return result;
	if (!absolute && timeout_address)
		restarted = futex_restart_take(
			current, address, private, expected, timeout_address,
			bitset, &deadline);
	timed = restarted ? 1 : absolute ?
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
		futex_restart_cancel(current);
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
	if (timed && !absolute) {
		if (!restarted) {
			uint64 delta = ktime_ms_to_ticks(milliseconds);
			uint64 now = ktime_get_ticks();

			deadline = now > ~(uint64)0 - delta ?
				~(uint64)0 : now + delta;
			futex_restart_set(current, address, private, expected,
			                  timeout_address, bitset, deadline);
		}
		result = wait_queue_sleep_interruptible_until(
			&slot->wait, &futex_table.lock, deadline);
	} else if (timed) {
		result = wait_queue_sleep_interruptible_timeout(
			&slot->wait, &futex_table.lock, milliseconds);
	} else {
		result = wait_queue_sleep_interruptible(
			&slot->wait, &futex_table.lock);
	}
	slot = current->wait_private;
	if (!slot)
		PANIC("futex waiter lost slot");
	futex_slot_put_locked(slot);
	current->wait_private = 0;
	current->wait_bitset = ~(uint32)0;
	spinlock_release(&futex_table.lock);
	if (result == WAIT_QUEUE_INTERRUPTED)
		return -SIGNAL_RESTART_SYS;
	futex_restart_cancel(current);
	return result == WAIT_QUEUE_TIMEOUT ? -LINUX_ETIMEDOUT : 0;
}

/* Wake matching waiters for @key; futex_table.lock is held. */
static int futex_wake_key_locked(const struct futex_key *key, int count,
				 uint32 bitset)
{
	struct futex_slot *slot;

	slot = futex_slot_find_locked(key);
	if (!slot)
		return 0;
	return wait_queue_wake_mask(&slot->wait, count, bitset);
}

/* Resolve a key and wake up to @count matching waiters. */
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

/* Wake source waiters, then move the remainder to a distinct destination. */
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

/**
 * sys_linux_futex() - Implement the supported Linux futex operations
 *
 * Context: User syscall context.
 * Return: Operation result or a negative Linux errno. Supported waits may be
 * signal-restarted; unsupported commands fail.
 */
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

/**
 * sys_linux_set_robust_list() - Register the current thread's robust list
 *
 * Context: User syscall context.
 * Return: Zero or a negative Linux errno.
 */
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

/**
 * sys_linux_get_robust_list() - Return a thread's registered robust-list head
 *
 * Context: User syscall context.
 * Return: Zero or a negative Linux errno.
 */
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

/* Wake one private and one shared waiter after a robust-word state change. */
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

/* Mark a dead owner's robust word and wake its waiters when accessible. */
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

/* Walk bounded robust-list storage before its address space is destroyed. */
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

/* Initialize all table slots before threads can issue futex syscalls. */
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
