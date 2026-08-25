#ifndef __CAFFEINIX_KERNEL_FUTEX_H
#define __CAFFEINIX_KERNEL_FUTEX_H

#include <thread.h>

void futex_init(void);
void futex_thread_exit(thread_t thread);
void futex_restart_cancel(thread_t thread);
void futex_restart_signal(thread_t thread, int through_handler);
void futex_restart_sigreturn(thread_t thread);

#endif
