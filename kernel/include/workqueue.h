#ifndef __CAFFEINIX_KERNEL_WORKQUEUE_H
#define __CAFFEINIX_KERNEL_WORKQUEUE_H

#include <list.h>
#include <typedefs.h>
#include <wait.h>

struct work_struct;

typedef void (*work_func_t)(struct work_struct *work);

struct work_struct {
	struct list node;
	work_func_t function;
	uint8 pending;
	uint8 running;
	struct wait_queue completion;
};

void workqueue_init(void);
void work_init(struct work_struct *work, work_func_t function);
int schedule_work(struct work_struct *work);
int cancel_work(struct work_struct *work);
int cancel_work_sync(struct work_struct *work);

#endif
