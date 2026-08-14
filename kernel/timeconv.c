#include <ktime.h>

uint64 ktime_ticks_to_ns(uint64 ticks, uint32 frequency)
{
	uint64 seconds;
	uint64 nanoseconds;

	if (!frequency)
		return 0;
	seconds = ticks / frequency;
	if (seconds > ~(uint64)0 / NSEC_PER_SEC)
		return ~(uint64)0;
	nanoseconds = seconds * NSEC_PER_SEC;
	nanoseconds += ticks % frequency * NSEC_PER_SEC / frequency;
	return nanoseconds;
}
