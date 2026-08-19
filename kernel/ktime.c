#include <ktime.h>
#include <riscv.h>
#include <timer.h>

#define MSEC_PER_SEC 1000ULL

static uint64 boot_ticks;
static uint64 realtime_base_ns;
static uint64 realtime_base_ticks;
static uint8 realtime_ready;

void ktime_boot_init(uint64 ticks)
{
	boot_ticks = ticks;
}

void ktime_set_realtime_ns(uint64 nanoseconds)
{
	realtime_base_ns = nanoseconds;
	realtime_base_ticks = time_r();
	__atomic_store_n(&realtime_ready, 1, __ATOMIC_RELEASE);
}

uint64 ktime_get_ticks(void)
{
	return time_r();
}

uint64 ktime_get_ms(void)
{
	uint64 ticks = time_r();
	uint64 frequency = timer_frequency();

	return ticks / frequency * MSEC_PER_SEC +
	       ticks % frequency * MSEC_PER_SEC / frequency;
}

uint64 ktime_get_ns(void)
{
	return ktime_ticks_to_ns(time_r(), timer_frequency());
}

uint64 ktime_get_boot_ns(void)
{
	return ktime_ticks_to_ns(time_r() - boot_ticks,
				 timer_frequency());
}

int ktime_get_realtime_ns(uint64 *nanoseconds)
{
	uint64 elapsed;

	if (!nanoseconds ||
	    !__atomic_load_n(&realtime_ready, __ATOMIC_ACQUIRE))
		return -1;
	elapsed = ktime_ticks_to_ns(time_r() - realtime_base_ticks,
				    timer_frequency());
	if (realtime_base_ns > ~(uint64)0 - elapsed)
		*nanoseconds = ~(uint64)0;
	else
		*nanoseconds = realtime_base_ns + elapsed;
	return 0;
}

uint64 ktime_ms_to_ticks(uint64 milliseconds)
{
	uint64 frequency = timer_frequency();
	uint64 seconds = milliseconds / MSEC_PER_SEC;
	uint64 remainder = milliseconds % MSEC_PER_SEC;
	uint64 fraction;
	uint64 partial;
	uint64 ticks;

	if (seconds > ~(uint64)0 / frequency)
		return ~(uint64)0;
	ticks = seconds * frequency;
	partial = frequency / MSEC_PER_SEC * remainder;
	fraction = frequency % MSEC_PER_SEC * remainder;
	partial += fraction / MSEC_PER_SEC;
	if (fraction % MSEC_PER_SEC)
		partial++;
	if (ticks > ~(uint64)0 - partial)
		return ~(uint64)0;
	return ticks + partial;
}

uint64 ktime_ns_to_ticks(uint64 nanoseconds)
{
	uint64 frequency = timer_frequency();
	uint64 seconds = nanoseconds / NSEC_PER_SEC;
	uint64 remainder = nanoseconds % NSEC_PER_SEC;
	uint64 fraction;
	uint64 partial;
	uint64 ticks;

	if (seconds > ~(uint64)0 / frequency)
		return ~(uint64)0;
	ticks = seconds * frequency;
	partial = frequency / NSEC_PER_SEC * remainder;
	fraction = frequency % NSEC_PER_SEC * remainder;
	partial += fraction / NSEC_PER_SEC;
	if (fraction % NSEC_PER_SEC)
		partial++;
	if (ticks > ~(uint64)0 - partial)
		return ~(uint64)0;
	return ticks + partial;
}
