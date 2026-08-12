#ifndef __CAFFEINIX_KERNEL_SLEEP_LOCK_H
#define __CAFFEINIX_KERNEL_SLEEP_LOCK_H

#include <spinlock.h>
#include <wait.h>

struct thread;

typedef struct sleeplock{
        uint8 locked;
        struct spinlock lk;
        struct wait_queue waiters;

        const char* name;
        struct thread *owner;
}*sleeplock_t;

void sleeplock_init(sleeplock_t lk, const char* name);
uint8 sleeplock_holding(sleeplock_t lk);
void sleeplock_acquire(sleeplock_t lk);
void sleeplock_release(sleeplock_t lk);

#endif
