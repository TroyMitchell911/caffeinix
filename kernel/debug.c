/*
 * On-demand diagnostic snapshots of CPUs, threads, memory, and I/O.
 *
 * Serial break queues a work item so snapshot operations are not run in the
 * UART interrupt handler. A guard coalesces concurrent dumps; unlocked
 * thread/CPU fields are best-effort observations, not a globally consistent
 * scheduler snapshot.
 */
#include <block_device.h>
#include <cpu.h>
#include <debug.h>
#include <ext4fs.h>
#include <page_cache.h>
#include <palloc.h>
#include <printf.h>
#include <scheduler.h>
#include <thread.h>
#include <wait.h>
#include <workqueue.h>

static volatile uint8 dumping;
static struct work_struct dump_work;

/*
 * Workqueue callback that moves the requested snapshot out of hard IRQ
 * context; the work item has permanent storage.
 */
static void debug_dump_work(struct work_struct *work)
{
	(void)work;
	debug_dump_state();
}

/**
 * debug_init() - Initialize the permanent diagnostic work item
 *
 * Context: Boot after workqueue initialization, before serial-break requests.
 */
void debug_init(void)
{
	work_init(&dump_work, debug_dump_work);
}

/**
 * debug_dump_state_request() - Queue a deferred diagnostic snapshot
 *
 * Repeated requests may coalesce on the single permanent work item.
 *
 * Context: Thread or interrupt context after debug_init(); does not sleep.
 */
void debug_dump_state_request(void)
{
	schedule_work(&dump_work);
}

/*
 * Validate table membership and element alignment before diagnostic code
 * dereferences a possibly stale scheduler thread pointer.
 */
static int thread_pointer_valid(thread_t candidate)
{
	uint64 address = (uint64)candidate;
	uint64 start = (uint64)&thread[0];
	uint64 end = (uint64)&thread[NTHREAD];

	return address >= start && address < end &&
	       !((address - start) % sizeof(*candidate));
}

/*
 * Return a best-effort ID only for a pointer within the fixed thread table;
 * invalid pointers are rendered as -1.
 */
static int thread_id(thread_t candidate)
{
	return thread_pointer_valid(candidate) ? candidate->tid : -1;
}

/*
 * Render a snapshot state without indexing an unchecked enum value; racing or
 * corrupt values are labeled invalid.
 */
static const char *thread_state_name(thread_state_t state)
{
	switch (state) {
	case THREAD_UNUSED:
		return "unused";
	case THREAD_ALLOCATED:
		return "allocated";
	case THREAD_RUNNABLE:
		return "runnable";
	case THREAD_RUNNING:
		return "running";
	case THREAD_SLEEPING:
		return "sleeping";
	case THREAD_EXITED:
		return "exited";
	default:
		return "invalid";
	}
}

/**
 * debug_dump_state() - Emit a best-effort system state snapshot
 *
 * Concurrent dumps are coalesced. Thread and CPU fields are sampled without a
 * global stop, so relationships may change while output is emitted.
 *
 * Context: Worker/thread context; may acquire subsystem locks. Do not invoke
 *          while holding the locks of subsystems being inspected.
 */
void debug_dump_state(void)
{
	struct page_cache_stats page_cache_stats;
	uint64 free_pages, total_pages;
	thread_t current, selected;
	int index;

	if (__sync_lock_test_and_set(&dumping, 1))
		return;
	printf_emergency("DEBUG_STATE_BEGIN cpu=%d tp=%p\n", cpuid(),
			 tp_r());
	for (index = 0; index < cpu_count(); index++) {
		current = cpus[index]->current;
		selected = cpus[index]->selected;
		printf_emergency("cpu=%d hart=%p current=%p tid=%d "
				 "selected=%p selected_tid=%d idle=%d locks=%d\n",
				 index, cpus[index]->hart_id, (uint64)current,
				 thread_id(current), (uint64)selected,
				 thread_id(selected), cpus[index]->idle,
				 cpus[index]->lock_nest_depth);
	}
	for (index = 0; index < NTHREAD; index++) {
		if (thread[index].state == THREAD_UNUSED)
			continue;
		printf_emergency("thread=%d tid=%d name=%s state=%s rq=%d "
				 "wait=%p timeout=%d\n", index,
				 thread[index].tid, thread[index].name,
				 thread_state_name(thread[index].state),
				 thread[index].sched.on_runqueue,
				 (uint64)thread[index].waiting_on,
				 thread[index].on_timeout_queue);
	}
	page_cache_get_stats(&page_cache_stats);
	total_pages = palloc_usable_bytes() / PGSIZE;
	free_pages = palloc_free_pages();
	printf_emergency("memory pages=%lu free=%lu used=%lu\n",
			 total_pages, free_pages, total_pages - free_pages);
	printf_emergency("page-cache pages=%lu hits=%lu misses=%lu "
			 "reclaimed=%lu mapped=%lu shared=%lu refs=%lu\n",
			 page_cache_stats.pages,
			 page_cache_stats.hits, page_cache_stats.misses,
			 page_cache_stats.reclaimed,
			 page_cache_stats.mapped_pages,
			 page_cache_stats.shared_pages,
			 page_cache_stats.mapping_references);
	ext4fs_debug_dump();
	virtio_blk_debug_dump();
	printf_emergency("DEBUG_STATE_END\n");
	__sync_lock_release(&dumping);
}
