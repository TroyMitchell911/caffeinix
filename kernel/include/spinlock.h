#ifndef __CAFFEINIX_KERNEL_SPIN_LOCK_H
#define __CAFFEINIX_KERNEL_SPIN_LOCK_H

#include <typedefs.h>

struct cpu;
typedef struct cpu *cpu_t;

typedef struct spinlock {
        /* Is the lock held? */
        uint8 locked;
        const char *name;
        cpu_t cpu;
}*spinlock_t;

void spinlock_init(spinlock_t lock, const char* name);
void spinlock_acquire(spinlock_t lock);
void spinlock_release(spinlock_t lock);
int spinlock_holding(spinlock_t lock);
void enter_critical(void);
void exit_critical(void);

#endif