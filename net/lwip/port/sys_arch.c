/*
 * lwIP sys_arch port for Caffeinix synchronization, time, and threads.
 *
 * Dynamic objects use kernel wait queues and locks; lightweight protection
 * tracks nesting per CPU while preemption is disabled around the transition.
 */
#include <cpu.h>
#include <debug.h>
#include <kernel_config.h>
#include <ktime.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <mystring.h>
#include <palloc.h>
#include <scheduler.h>
#include <sleeplock.h>
#include <spinlock.h>
#include <thread.h>
#include <vfs.h>
#include <wait.h>

#define LWIP_SYS_MBOX_MAX 128

struct lwip_sys_sem {
	struct spinlock lock;
	struct wait_queue wait;
	uint32 count;
};

struct lwip_sys_mutex {
	struct sleeplock lock;
};

struct lwip_sys_mbox {
	struct spinlock lock;
	struct wait_queue not_empty;
	struct wait_queue not_full;
	uint32 size;
	uint32 head;
	uint32 tail;
	uint32 count;
	void *messages[];
};

static struct {
	struct spinlock lock;
	uint32 *depth;
} lightweight_protection;

static struct spinlock random_lock;
static uint32 random_state;
static struct lwip_sys_sem thread_semaphore_storage[NTHREAD];
static sys_sem_t thread_semaphores[NTHREAD];

int *lwip_errno_location(void)
{
	return &cur_thread()->lwip_errno;
}

sys_sem_t *lwip_thread_sem(void)
{
	thread_t current = cur_thread();

	if (!current || current < thread || current >= thread + NTHREAD)
		PANIC("invalid lwIP thread");
	return &thread_semaphores[current - thread];
}

/**
 * sys_init() - Initialize this port before lwIP creates its tcpip thread
 *
 * Context: Early process context, once; allocates per-CPU protection state.
 */
void sys_init(void)
{
	uint32 index;

	lightweight_protection.depth = calloc(
		cpu_count(), sizeof(*lightweight_protection.depth));
	if (!lightweight_protection.depth)
		PANIC("allocate lwIP CPU state");
	spinlock_init(&lightweight_protection.lock, "lwIP protect");
	spinlock_init(&random_lock, "lwIP random");
	for (index = 0; index < NTHREAD; index++) {
		struct lwip_sys_sem *semaphore =
			&thread_semaphore_storage[index];

		spinlock_init(&semaphore->lock, "lwIP thread semaphore");
		wait_queue_init(&semaphore->wait,
				"lwIP thread semaphore");
		semaphore->count = 0;
		thread_semaphores[index] = semaphore;
	}
	random_state = (uint32)ktime_get_ticks() ^ 0x9e3779b9U;
}

/**
 * sys_mutex_new() - Allocate a sleepable mutex
 * @mutex: Non-NULL output handle.
 *
 * Context: Process context; may allocate.
 *
 * Return: ERR_OK, ERR_ARG, or ERR_MEM.
 */
err_t sys_mutex_new(sys_mutex_t *mutex)
{
	if (!mutex)
		return ERR_ARG;
	*mutex = calloc(1, sizeof(**mutex));
	if (!*mutex)
		return ERR_MEM;
	sleeplock_init(&(*mutex)->lock, "lwIP mutex");
	return ERR_OK;
}

/**
 * sys_mutex_lock() - Acquire a valid lwIP mutex
 * @mutex: Valid mutex handle.
 *
 * Context: Thread context; may sleep.
 */
void sys_mutex_lock(sys_mutex_t *mutex)
{
	if (!sys_mutex_valid(mutex))
		PANIC("invalid lwIP mutex");
	sleeplock_acquire(&(*mutex)->lock);
}

/**
 * sys_mutex_unlock() - Release a mutex acquired by the current thread
 * @mutex: Valid mutex handle.
 *
 * Context: Thread context; does not sleep.
 */
void sys_mutex_unlock(sys_mutex_t *mutex)
{
	if (!sys_mutex_valid(mutex))
		PANIC("invalid lwIP mutex");
	sleeplock_release(&(*mutex)->lock);
}

/**
 * sys_mutex_free() - Free an idle mutex and clear its handle
 * @mutex: Valid or NULL mutex handle.
 *
 * Context: Process context; no holder or waiters may remain.
 */
void sys_mutex_free(sys_mutex_t *mutex)
{
	if (!sys_mutex_valid(mutex))
		return;
	if ((*mutex)->lock.locked ||
	    !wait_queue_empty(&(*mutex)->lock.waiters))
		PANIC("free busy lwIP mutex");
	free(*mutex);
	*mutex = 0;
}

/**
 * sys_mutex_valid() - Test whether a mutex handle names an allocated object
 * @mutex: Candidate handle.
 *
 * Context: Any context.
 *
 * Return: Nonzero for a valid handle.
 */
int sys_mutex_valid(sys_mutex_t *mutex)
{
	return mutex && *mutex;
}

/**
 * sys_mutex_set_invalid() - Clear a mutex handle without freeing it
 * @mutex: Optional handle storage.
 *
 * Context: Any context.
 */
void sys_mutex_set_invalid(sys_mutex_t *mutex)
{
	if (mutex)
		*mutex = 0;
}

/**
 * sys_sem_new() - Allocate a counting semaphore
 * @sem: Non-NULL output handle.
 * @count: Initial token count.
 *
 * Context: Process context; may allocate.
 *
 * Return: ERR_OK, ERR_ARG, or ERR_MEM.
 */
err_t sys_sem_new(sys_sem_t *sem, u8_t count)
{
	if (!sem)
		return ERR_ARG;
	*sem = calloc(1, sizeof(**sem));
	if (!*sem)
		return ERR_MEM;
	spinlock_init(&(*sem)->lock, "lwIP semaphore");
	wait_queue_init(&(*sem)->wait, "lwIP semaphore");
	(*sem)->count = count;
	return ERR_OK;
}

/**
 * sys_sem_signal() - Increment a valid semaphore and wake one waiter
 * @sem: Valid semaphore handle.
 *
 * Context: Sleepable process or deferred-work context; does not sleep.
 */
void sys_sem_signal(sys_sem_t *sem)
{
	if (!sys_sem_valid(sem))
		PANIC("invalid lwIP semaphore");
	spinlock_acquire(&(*sem)->lock);
	(*sem)->count++;
	wait_queue_wake_one(&(*sem)->wait);
	spinlock_release(&(*sem)->lock);
}

/**
 * sys_arch_sem_wait() - Consume a semaphore token or return timeout
 * @sem: Valid semaphore handle.
 * @timeout: Milliseconds, or zero for an unbounded wait.
 *
 * Context: Thread context; may sleep.
 *
 * Return: Elapsed milliseconds or timeout.
 */
u32_t sys_arch_sem_wait(sys_sem_t *sem, u32_t timeout)
{
	uint32 elapsed, start;
	int result;

	if (!sys_sem_valid(sem))
		PANIC("invalid lwIP semaphore");
	start = sys_now();
	spinlock_acquire(&(*sem)->lock);
	while (!(*sem)->count) {
		if (!timeout) {
			wait_queue_sleep(&(*sem)->wait, &(*sem)->lock);
			continue;
		}
		elapsed = sys_now() - start;
		if (elapsed >= timeout) {
			spinlock_release(&(*sem)->lock);
			return SYS_ARCH_TIMEOUT;
		}
		result = wait_queue_sleep_timeout(
			&(*sem)->wait, &(*sem)->lock, timeout - elapsed);
		if (result < 0 && !(*sem)->count) {
			spinlock_release(&(*sem)->lock);
			return SYS_ARCH_TIMEOUT;
		}
	}
	(*sem)->count--;
	spinlock_release(&(*sem)->lock);
	return sys_now() - start;
}

/**
 * sys_sem_free() - Free an idle semaphore and invalidate its handle
 * @sem: Valid or NULL handle.
 *
 * Context: Process context; no waiters may remain.
 */
void sys_sem_free(sys_sem_t *sem)
{
	if (!sys_sem_valid(sem))
		return;
	if (!wait_queue_empty(&(*sem)->wait))
		PANIC("free busy lwIP semaphore");
	free(*sem);
	*sem = 0;
}

/**
 * sys_sem_valid() - Test whether a semaphore handle is valid
 * @sem: Candidate handle.
 *
 * Context: Any context.
 *
 * Return: Nonzero for a valid handle.
 */
int sys_sem_valid(sys_sem_t *sem)
{
	return sem && *sem;
}

/**
 * sys_sem_set_invalid() - Clear a semaphore handle without freeing storage
 * @sem: Optional handle storage.
 *
 * Context: Any context.
 */
void sys_sem_set_invalid(sys_sem_t *sem)
{
	if (sem)
		*sem = 0;
}

/**
 * sys_mbox_new() - Allocate a bounded FIFO mailbox
 * @mbox: Non-NULL output handle.
 * @size: Requested entries, normalized to the supported bound.
 *
 * Context: Process context; may allocate.
 *
 * Return: ERR_OK, ERR_ARG, or ERR_MEM.
 */
err_t sys_mbox_new(sys_mbox_t *mbox, int size)
{
	uint64 allocation;

	if (!mbox)
		return ERR_ARG;
	if (size <= 0)
		size = 16;
	if (size > LWIP_SYS_MBOX_MAX)
		return ERR_ARG;
	allocation = sizeof(**mbox) + sizeof(void *) * size;
	*mbox = calloc(1, allocation);
	if (!*mbox)
		return ERR_MEM;
	spinlock_init(&(*mbox)->lock, "lwIP mailbox");
	wait_queue_init(&(*mbox)->not_empty, "lwIP mailbox receive");
	wait_queue_init(&(*mbox)->not_full, "lwIP mailbox transmit");
	(*mbox)->size = size;
	return ERR_OK;
}

/**
 * sys_mbox_post() - Enqueue a message, sleeping until room exists
 * @mbox: Valid mailbox handle.
 * @message: Opaque pointer retained until a receiver dequeues it.
 *
 * Context: Thread context; may sleep.
 */
void sys_mbox_post(sys_mbox_t *mbox, void *message)
{
	if (!sys_mbox_valid(mbox))
		PANIC("invalid lwIP mailbox");
	spinlock_acquire(&(*mbox)->lock);
	while ((*mbox)->count == (*mbox)->size)
		wait_queue_sleep(&(*mbox)->not_full, &(*mbox)->lock);
	(*mbox)->messages[(*mbox)->head++ % (*mbox)->size] = message;
	(*mbox)->count++;
	wait_queue_wake_one(&(*mbox)->not_empty);
	spinlock_release(&(*mbox)->lock);
}

/**
 * sys_mbox_trypost() - Enqueue a message without sleeping
 * @mbox: Valid mailbox handle.
 * @message: Opaque pointer to enqueue.
 *
 * Context: Any non-hard-IRQ context.
 *
 * Return: ERR_OK, ERR_ARG, or ERR_MEM.
 */
err_t sys_mbox_trypost(sys_mbox_t *mbox, void *message)
{
	if (!sys_mbox_valid(mbox))
		return ERR_ARG;
	spinlock_acquire(&(*mbox)->lock);
	if ((*mbox)->count == (*mbox)->size) {
		spinlock_release(&(*mbox)->lock);
		return ERR_MEM;
	}
	(*mbox)->messages[(*mbox)->head++ % (*mbox)->size] = message;
	(*mbox)->count++;
	wait_queue_wake_one(&(*mbox)->not_empty);
	spinlock_release(&(*mbox)->lock);
	return ERR_OK;
}

/**
 * sys_mbox_trypost_fromisr() - Nonblocking lwIP ISR-style mailbox post
 * @mbox: Valid mailbox handle.
 * @message: Opaque pointer to enqueue.
 *
 * Context: Deferred context;
 *
 * Return: ERR_OK, ERR_ARG, or ERR_MEM.
 */
err_t sys_mbox_trypost_fromisr(sys_mbox_t *mbox, void *message)
{
	return sys_mbox_trypost(mbox, message);
}

/**
 * sys_arch_mbox_fetch() - Dequeue a message, blocking up to a timeout
 * @mbox: Valid mailbox handle.
 * @message: Optional output pointer.
 * @timeout: Milliseconds, or zero for an unbounded wait.
 *
 * Context: Thread context; may sleep.
 *
 * Return: Elapsed milliseconds or timeout.
 */
u32_t sys_arch_mbox_fetch(sys_mbox_t *mbox, void **message,
			  u32_t timeout)
{
	uint32 elapsed, start;
	void *value;
	int result;

	if (!sys_mbox_valid(mbox))
		PANIC("invalid lwIP mailbox");
	start = sys_now();
	spinlock_acquire(&(*mbox)->lock);
	while (!(*mbox)->count) {
		if (!timeout) {
			wait_queue_sleep(&(*mbox)->not_empty, &(*mbox)->lock);
			continue;
		}
		elapsed = sys_now() - start;
		if (elapsed >= timeout) {
			spinlock_release(&(*mbox)->lock);
			return SYS_ARCH_TIMEOUT;
		}
		result = wait_queue_sleep_timeout(
			&(*mbox)->not_empty, &(*mbox)->lock,
			timeout - elapsed);
		if (result < 0 && !(*mbox)->count) {
			spinlock_release(&(*mbox)->lock);
			return SYS_ARCH_TIMEOUT;
		}
	}
	value = (*mbox)->messages[(*mbox)->tail++ % (*mbox)->size];
	(*mbox)->count--;
	wait_queue_wake_one(&(*mbox)->not_full);
	spinlock_release(&(*mbox)->lock);
	if (message)
		*message = value;
	return sys_now() - start;
}

/**
 * sys_arch_mbox_tryfetch() - Dequeue immediately or report an empty mailbox
 * @mbox: Valid mailbox handle.
 * @message: Optional output pointer.
 *
 * Context: Any non-hard-IRQ context.
 *
 * Return: 0 or SYS_MBOX_EMPTY.
 */
u32_t sys_arch_mbox_tryfetch(sys_mbox_t *mbox, void **message)
{
	void *value;

	if (!sys_mbox_valid(mbox))
		return SYS_MBOX_EMPTY;
	spinlock_acquire(&(*mbox)->lock);
	if (!(*mbox)->count) {
		spinlock_release(&(*mbox)->lock);
		return SYS_MBOX_EMPTY;
	}
	value = (*mbox)->messages[(*mbox)->tail++ % (*mbox)->size];
	(*mbox)->count--;
	wait_queue_wake_one(&(*mbox)->not_full);
	spinlock_release(&(*mbox)->lock);
	if (message)
		*message = value;
	return 0;
}

/**
 * sys_mbox_free() - Free an empty mailbox with no blocked users
 * @mbox: Valid or NULL mailbox handle.
 *
 * Context: Process context; mailbox ownership transfers back to allocator.
 */
void sys_mbox_free(sys_mbox_t *mbox)
{
	if (!sys_mbox_valid(mbox))
		return;
	if ((*mbox)->count || !wait_queue_empty(&(*mbox)->not_empty) ||
	    !wait_queue_empty(&(*mbox)->not_full))
		PANIC("free busy lwIP mailbox");
	free(*mbox);
	*mbox = 0;
}

/**
 * sys_mbox_valid() - Test whether a mailbox handle names an allocated FIFO
 * @mbox: Candidate handle.
 *
 * Context: Any context.
 *
 * Return: Nonzero for a valid handle.
 */
int sys_mbox_valid(sys_mbox_t *mbox)
{
	return mbox && *mbox;
}

/**
 * sys_mbox_set_invalid() - Clear a mailbox handle without freeing storage
 * @mbox: Optional handle storage.
 *
 * Context: Any context.
 */
void sys_mbox_set_invalid(sys_mbox_t *mbox)
{
	if (mbox)
		*mbox = 0;
}

/**
 * sys_thread_new() - Start a named Caffeinix thread for lwIP work
 * @name: Thread name.
 * @function: lwIP entry function.
 * @argument: Opaque function argument.
 * @stack_size: Ignored; kernel thread policy supplies the stack.
 * @priority: Ignored; scheduler policy supplies priority.
 *
 * Context: Process context; may allocate.
 *
 * Return: Created thread handle.
 */
sys_thread_t sys_thread_new(const char *name, lwip_thread_fn function,
			    void *argument, int stack_size, int priority)
{
	thread_t created;

	(void)stack_size;
	(void)priority;
	created = kernel_thread_create(name, function, argument);
	if (!created)
		PANIC("create lwIP thread");
	return created;
}

/**
 * sys_jiffies() - Return monotonic kernel timer ticks
 *
 * Context: Any context.
 *
 * Return: Current tick count truncated to lwIP width.
 */
u32_t sys_jiffies(void)
{
	return (u32_t)ktime_get_ticks();
}

/**
 * sys_now() - Return monotonic milliseconds used by lwIP timeouts
 *
 * Context: Any context.
 *
 * Return: Current millisecond count truncated to u32_t.
 */
u32_t sys_now(void)
{
	return (u32_t)ktime_get_ms();
}

/**
 * sys_arch_protect() - Enter recursive lightweight protection on this CPU
 *
 * Context: Any non-hard-IRQ context.
 *
 * Return: Previous nesting depth token.
 */
sys_prot_t sys_arch_protect(void)
{
	uint32 previous;
	int cpu;

	enter_critical();
	cpu = cpuid();
	if (cpu < 0 || cpu >= cpu_count())
		PANIC("invalid lwIP protect CPU");
	previous = lightweight_protection.depth[cpu];
	if (!previous)
		spinlock_acquire(&lightweight_protection.lock);
	lightweight_protection.depth[cpu] = previous + 1;
	exit_critical();
	return previous;
}

/**
 * sys_arch_unprotect() - Restore lightweight-protection nesting
 * @previous: Token returned by the matching sys_arch_protect().
 *
 * Context: Same CPU and context as the matching acquire.
 */
void sys_arch_unprotect(sys_prot_t previous)
{
	int cpu;

	enter_critical();
	cpu = cpuid();
	if (cpu < 0 || cpu >= cpu_count() ||
	    lightweight_protection.depth[cpu] != previous + 1)
		PANIC("unbalanced lwIP protection");
	lightweight_protection.depth[cpu] = previous;
	if (!previous)
		spinlock_release(&lightweight_protection.lock);
	exit_critical();
}

void lwip_socket_event_notify(void)
{
	vfs_poll_notify();
}

u32_t lwip_port_rand(void)
{
	uint32 value;

	spinlock_acquire(&random_lock);
	value = random_state;
	value ^= value << 13;
	value ^= value >> 17;
	value ^= value << 5;
	random_state = value;
	spinlock_release(&random_lock);
	return value;
}
