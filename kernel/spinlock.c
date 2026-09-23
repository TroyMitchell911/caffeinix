/*
 * RISC-V spin lock implementation.
 *
 * The lock word provides mutual exclusion while CPU ownership detects misuse.
 * enter_critical() disables local interrupts before lock acquisition so local
 * interrupt handlers cannot deadlock on a lock interrupted code already owns.
 *
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved.
 */
#include <spinlock.h>
#include <riscv.h>
#include <debug.h>
#include <scheduler.h>

void enter_critical(void)
{
	/*
	 * Only the outermost nesting level restores the saved interrupt state.
	 */
        int old = intr_status();
        intr_off();
        cpu_t cpu = cur_cpu();
        if(cpu->lock_nest_depth++ == 0) {
                cpu->before_lock = old;
        }
}

void exit_critical(void)
{
        cpu_t cpu = cur_cpu();
        if(intr_status()) {
                PANIC("exit_critical");
        }
        if(cpu->lock_nest_depth < 1) {
                PANIC("exit_critical");
        }
        if(--cpu->lock_nest_depth == 0 && cpu->before_lock) {
                intr_on();
        }
}

int spinlock_holding(spinlock_t lock)
{
	/*
	 * Ownership is diagnostic; callers still need locking for data access.
	 */
        return (lock->locked && lock->cpu == cur_cpu());
}

void spinlock_init(spinlock_t lock, const char* name)
{
        lock->name = name;
        lock->locked = 0;
        lock->cpu = 0;
}

void spinlock_acquire(spinlock_t lock)
{
        enter_critical();

        if(spinlock_holding(lock)) {
                printf("%s->", lock->name);
                PANIC("spainlock_acquire");
        }
                
	/* Atomic exchange serializes contenders before entering the section. */
        while(__sync_lock_test_and_set(&lock->locked, 1) != 0);
	/* Prevent protected loads and stores from moving before acquisition. */
        __sync_synchronize();
        lock->cpu = cur_cpu();
}

int spinlock_trylock(spinlock_t lock)
{
	int ret;

        enter_critical();

        if(spinlock_holding(lock)) {
                printf("%s->", lock->name);
                PANIC("spainlock_acquire");
        }

	ret = __sync_lock_test_and_set(&lock->locked, 1);
        __sync_synchronize();

	if (!ret)
		lock->cpu = cur_cpu();
	else
		exit_critical();

	return ret;
}

void spinlock_release(spinlock_t lock)
{
        if(!spinlock_holding(lock)) {
                printf("%s->", lock->name);
                PANIC("spinlock_release");
        }
       

        lock->cpu = 0;
	/* Publish protected stores before making the lock available. */
        __sync_synchronize();
	/* Use the atomic primitive required by the lock-word protocol. */
        __sync_lock_release(&lock->locked);
        exit_critical();
}
