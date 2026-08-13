#include <ktime.h>
#include <riscv.h>
#include <timer.h>

#define NSEC_PER_SEC 1000000000ULL
#define MSEC_PER_SEC 1000ULL

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
	uint64 ticks = time_r();
	uint64 frequency = timer_frequency();

	return ticks / frequency * NSEC_PER_SEC +
	       ticks % frequency * NSEC_PER_SEC / frequency;
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
