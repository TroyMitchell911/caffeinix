/*
 * Spin locks protect short, non-sleeping critical sections.
 *
 * Acquiring a lock disables local interrupts until the matching release, so
 * code holding one must neither sleep nor take a lock that can be acquired
 * from an interrupt handler in the opposite order.
 */
#ifndef __CAFFEINIX_KERNEL_SPIN_LOCK_H
#define __CAFFEINIX_KERNEL_SPIN_LOCK_H
#include <typedefs.h>

struct cpu;
typedef struct cpu *cpu_t;

typedef struct spinlock {
	/* Protected by the atomic lock operation; diagnostic ownership only. */
        uint8 locked;
        const char *name;
        cpu_t cpu;
}*spinlock_t;

/**
 * spinlock_init() - Initialize storage before first use
 * @lock: Caller-owned lock storage that is not concurrently accessible.
 * @name: Stable diagnostic name, or NULL when no name is available.
 *
 * Context: Early boot or externally serialized thread context. Does not
 * sleep. The storage must remain live until all users have stopped.
 */
void spinlock_init(spinlock_t lock, const char *name);

/**
 * spinlock_acquire() - Acquire a spin lock and disable local interrupts
 * @lock: Initialized lock to acquire.
 *
 * Context: Any context that may spin. Must not already hold @lock and must
 * not sleep until spinlock_release(). Lock ordering is the caller's duty.
 */
void spinlock_acquire(spinlock_t lock);

/**
 * spinlock_trylock() - Try to acquire a spin lock without waiting
 * @lock: Initialized lock to acquire.
 *
 * Context: Any non-sleeping context. On success local interrupts remain
 * disabled until spinlock_release(); on failure their prior state is kept.
 * Must not already hold @lock; recursive acquisition panics.
 *
 * Return: Zero on acquisition, nonzero when another CPU holds @lock.
 */
int spinlock_trylock(spinlock_t lock);

/**
 * spinlock_release() - Release a lock acquired by this CPU
 * @lock: Lock held by the current CPU.
 *
 * Context: Non-sleeping context. Pairs with spinlock_acquire() or a
 * successful spinlock_trylock() and restores the outer interrupt state.
 */
void spinlock_release(spinlock_t lock);

/**
 * spinlock_holding() - Test diagnostic ownership of a lock
 * @lock: Initialized lock.
 *
 * Context: Any non-sleeping context. This is a racy assertion helper, not a
 * synchronization primitive.
 *
 * Return: Nonzero when this CPU owns @lock.
 */
int spinlock_holding(spinlock_t lock);

/**
 * enter_critical() - Disable local interrupts and nest a critical section
 *
 * Context: Any context. Must be paired with exit_critical() on the same CPU.
 * Nested callers preserve the interrupt state observed by the outermost one.
 */
void enter_critical(void);

/**
 * exit_critical() - Leave a local interrupt-disabled critical section
 *
 * Context: Any context. Must pair with enter_critical() on the same CPU.
 * Restores interrupts only when leaving the outermost nesting level.
 */
void exit_critical(void);

#endif
