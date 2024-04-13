#include <spinlock.h>
#include <riscv.h>
#include <debug.h>

void enter_critical(void)
{
        cpu_t cpu = cur_cpu();
        int old = intr_status();
        intr_off();
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

        if(spinlock_holding(lock))
                PANIC("spainlock_acquire");

        while(__sync_lock_test_and_set(&lock->locked, 1) != 0);
        __sync_synchronize();
        lock->cpu = cur_cpu();
}

void spinlock_release(spinlock_t lock)
{
        if(!spinlock_holding(lock))
                PANIC("spinlock_release");

        lock->cpu = 0;
        __sync_synchronize();
        __sync_lock_release(&lock->locked);
        exit_critical();
}



