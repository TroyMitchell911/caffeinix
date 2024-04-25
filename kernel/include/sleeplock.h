#ifndef __CAFFEINIX_KERNEL_SLEEP_LOCK_H
#define __CAFFEINIX_KERNEL_SLEEP_LOCK_H

#include <spinlock.h>

typedef struct sleeplock{
        uint8 locked;
        struct spinlock lk;

        const char* name;
        /* Point a process that held this lock */
        void* p;
}*sleeplock_t;

void sleeplock_init(sleeplock_t lk, const char* name);
uint8 sleeplock_holding(sleeplock_t lk);
void sleeplock_acquire(sleeplock_t lk);
void sleeplock_release(sleeplock_t lk);

#endif