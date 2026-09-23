/* Caffeinix object types used by the imported lwIP sys_arch contract. */
#ifndef __CAFFEINIX_LWIP_ARCH_SYS_ARCH_H
#define __CAFFEINIX_LWIP_ARCH_SYS_ARCH_H

#include <typedefs.h>

struct lwip_sys_sem;
struct lwip_sys_mutex;
struct lwip_sys_mbox;

typedef struct lwip_sys_sem *sys_sem_t;
typedef struct lwip_sys_mutex *sys_mutex_t;
typedef struct lwip_sys_mbox *sys_mbox_t;
typedef void *sys_thread_t;
typedef uint64 sys_prot_t;

/**
 * lwip_thread_sem() - Return the permanent netconn semaphore for this thread
 *
 * The handle is permanent storage for the current thread slot and must not
 * be freed. An absent current thread or an invalid slot is fatal.
 *
 * Context: Valid thread context after sys_init(); does not sleep.
 *
 * Return: Address of the current thread's semaphore handle, never NULL.
 */
sys_sem_t *lwip_thread_sem(void);

#define LWIP_NETCONN_THREAD_SEM_GET() lwip_thread_sem()
#define LWIP_NETCONN_THREAD_SEM_ALLOC() ((void)0)
#define LWIP_NETCONN_THREAD_SEM_FREE() ((void)0)

#endif
