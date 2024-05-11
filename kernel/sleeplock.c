#include <sleeplock.h>
#include <process.h>
#include <scheduler.h>
#include <debug.h>

void sleeplock_init(sleeplock_t lk, const char* name)
{
        spinlock_init(&lk->lk, name);

        lk->name = name;
        lk->p = 0;
        lk->locked = 0;  
}

uint8 sleeplock_holding(sleeplock_t lk)
{
        uint8 r;
        
        spinlock_acquire(&lk->lk);

        r = (lk->locked && (cur_proc() == (process_t)lk->p));

        spinlock_release(&lk->lk);

        return r;
}

void sleeplock_acquire(sleeplock_t lk)
{
        spinlock_acquire(&lk->lk);

        while(lk->locked) {
                sleep(lk, &lk->lk);
        }
        lk->locked = 1;
        lk->p = cur_proc();
        spinlock_release(&lk->lk);
}

void sleeplock_release(sleeplock_t lk)
{
        if(!sleeplock_holding(lk)) {
                PANIC("sleeplock_release");
        }
        spinlock_acquire(&lk->lk);
        lk->locked = 0;
        lk->p = 0;
        wakeup(lk);
        spinlock_release(&lk->lk);    
}