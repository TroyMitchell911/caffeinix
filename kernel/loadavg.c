/*
 * Fixed-point 1, 5, and 15 minute load averages.
 *
 * A kernel worker samples runnable/running and uninterruptibly blocked work
 * every five seconds. The exported values are scaled by LOADAVG_FIXED; they
 * are load estimates, not CPU utilization percentages.
 */
#include <debug.h>
#include <loadavg.h>
#include <mystring.h>
#include <process.h>
#include <spinlock.h>
#include <thread.h>
#include <wait.h>

#define LOADAVG_PERIOD_MS 5000ULL

static struct {
	struct spinlock lock;
	struct wait_queue wait;
	uint32 average[3];
} load_average;

static const uint32 loadavg_exponent[] = { 1884, 2014, 2037 };

/*
 * Exponentiate the scaled decay factor by squaring; every product is rounded
 * back to LOADAVG_FIXED units, avoiding floating-point kernel state.
 */
static uint32 loadavg_fixed_power(uint32 base, uint64 exponent)
{
	uint64 result = LOADAVG_FIXED;
	uint64 factor = base;

	while (exponent) {
		if (exponent & 1)
			result = (result * factor + LOADAVG_FIXED / 2) /
				 LOADAVG_FIXED;
		factor = (factor * factor + LOADAVG_FIXED / 2) /
			 LOADAVG_FIXED;
		exponent >>= 1;
	}
	return result;
}

/*
 * Blend the old scaled average with the active-task target using the elapsed
 * number of five-second decay periods.
 */
static uint32 loadavg_update(uint32 old, uint32 active,
			     uint32 exponent, uint64 periods)
{
	uint32 decay = loadavg_fixed_power(exponent, periods);
	uint64 target = (uint64)active * LOADAVG_FIXED;

	return (old * (uint64)decay +
		target * (LOADAVG_FIXED - decay) + LOADAVG_FIXED / 2) /
	       LOADAVG_FIXED;
}

/*
 * Obtain the process snapshot without the average lock, then update all three
 * averages together so readers never observe a mixed sample.
 */
static void loadavg_sample(void)
{
	struct process_system_snapshot snapshot;
	uint32 active;
	int index;

	process_snapshot_system(&snapshot);
	active = snapshot.running + snapshot.blocked;
	spinlock_acquire(&load_average.lock);
	for (index = 0; index < (int)NELEM(load_average.average); index++)
		load_average.average[index] = loadavg_update(
			load_average.average[index], active,
			loadavg_exponent[index], 1);
	spinlock_release(&load_average.lock);
}

/*
 * Sleep on the worker-private timed queue between samples. The worker runs
 * indefinitely and must drop its condition lock before collecting process
 * statistics.
 */
static void loadavg_thread(void *argument)
{
	(void)argument;
	for (;;) {
		spinlock_acquire(&load_average.lock);
		(void)wait_queue_sleep_timeout(&load_average.wait,
					       &load_average.lock,
					       LOADAVG_PERIOD_MS);
		spinlock_release(&load_average.lock);
		loadavg_sample();
	}
}

/**
 * loadavg_get() - Copy the three scaled load estimates
 * @average: Kernel array of three uint32 values; NULL is ignored.
 *
 * Elements are ordered as 1, 5, and 15 minute averages; divide by
 * LOADAVG_FIXED to obtain the load.
 *
 * Context: After initialization; takes the load-average spinlock, does not
 *          sleep.
 */
void loadavg_get(uint32 average[3])
{
	int index;

	if (!average)
		return;
	spinlock_acquire(&load_average.lock);
	for (index = 0; index < 3; index++)
		average[index] = load_average.average[index];
	spinlock_release(&load_average.lock);
}

/**
 * loadavg_init() - Start periodic load sampling
 *
 * Initializes the averages to zero and creates one persistent worker. Worker
 * creation failure is fatal.
 *
 * Context: Boot thread after wait queues and kernel threads are initialized.
 */
void loadavg_init(void)
{
	spinlock_init(&load_average.lock, "load average");
	wait_queue_init(&load_average.wait, "load average");
	memset(load_average.average, 0, sizeof(load_average.average));
	if (!kernel_thread_create("kloadavg", loadavg_thread, 0))
		PANIC("create load average worker");
}
