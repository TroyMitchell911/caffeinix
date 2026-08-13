#include <sleeplock.h>
#include <scheduler.h>
#include <debug.h>
#include <wait.h>

void sleeplock_init(sleeplock_t lk, const char* name)
{
        spinlock_init(&lk->lk, name);
        wait_queue_init(&lk->waiters, name);

        lk->name = name;
        lk->owner = 0;
        lk->locked = 0;  
}

uint8 sleeplock_holding(sleeplock_t lk)
{
        uint8 r;
        
        spinlock_acquire(&lk->lk);

        r = lk->locked && cur_thread() == lk->owner;

        spinlock_release(&lk->lk);

        return r;
}

void sleeplock_acquire(sleeplock_t lk)
{
        spinlock_acquire(&lk->lk);

        while(lk->locked) {
                wait_queue_sleep(&lk->waiters, &lk->lk);
        }
        lk->locked = 1;
        lk->owner = cur_thread();
        spinlock_release(&lk->lk);
}

void sleeplock_release(sleeplock_t lk)
{
        spinlock_acquire(&lk->lk);
        if(!lk->locked || lk->owner != cur_thread())
                PANIC("sleeplock_release");
        lk->locked = 0;
        lk->owner = 0;
        wait_queue_wake_one(&lk->waiters);
        spinlock_release(&lk->lk);    
}
