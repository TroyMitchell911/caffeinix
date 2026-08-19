#ifndef __CAFFEINIX_KERNEL_FUTEX_H
#define __CAFFEINIX_KERNEL_FUTEX_H

#include <thread.h>

void futex_init(void);
void futex_thread_exit(thread_t thread);

#endif
