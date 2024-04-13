#ifndef __CAFFEINIX_KERNEL_SPIN_LOCK_H
#define __CAFFEINIX_KERNEL_SPIN_LOCK_H

#include <typedefs.h>
#include <thread.h>

typedef struct spinlock {
        /* Is the lock held? */
        uint8 locked;
        const char *name;
        cpu_t cpu;
}*spinlock_t;

#endif