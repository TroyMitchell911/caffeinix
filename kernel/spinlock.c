#include <spinlock.h>
#include <riscv.h>
#include <debug.h>
#include <scheduler.h>

void enter_critical(void)
{
        /* Get the interrupt status */
        int old = intr_status();
        intr_off();
        /* Call the function when the interrupt disabled */
        cpu_t cpu = cur_cpu();
        /* Add the depth */
        if(cpu->lock_nest_depth++ == 0) {
                cpu->before_lock = old;
        }
}

void exit_critical(void)
{
        cpu_t cpu = cur_cpu();
        /* We shouldn't open the interrupt when we exit the critical */
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
        /* If locked and the CPU that obtained the lock is the current CPU, return 1 */
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
        /*
                On RISC-V, sync_lock_test_and_set turns into an atomic swap:
                a5 = 1
                s1 = &lk->locked
                amoswap.w.aq a5, a5, (s1) 
        */
        while(__sync_lock_test_and_set(&lock->locked, 1) != 0);
        /*
                Tell the C compiler and the processor to not move loads or stores
                past this point, to ensure that the critical section's memory
                references happen strictly after the lock is acquired.
                On RISC-V, this emits a fence instruction. 
        */
        __sync_synchronize();
        lock->cpu = cur_cpu();
}

void spinlock_release(spinlock_t lock)
{
        if(!spinlock_holding(lock))
                PANIC("spinlock_release");

        lock->cpu = 0;
        /*
                Tell the C compiler and the processor to not move loads or stores
                past this point, to ensure that the critical section's memory
                references happen strictly after the lock is acquired.
                On RISC-V, this emits a fence instruction. 
        */
        __sync_synchronize();
        /*
                Release the lock, equivalent to lk->locked = 0.
                This code doesn't use a C assignment, since the C standard
                implies that an assignment might be implemented with
                multiple store instructions.
                On RISC-V, sync_lock_release turns into an atomic swap:
                s1 = &lk->locked
                amoswap.w zero, zero, (s1) */
        __sync_lock_release(&lock->locked);
        exit_critical();
}



