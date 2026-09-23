/*
 * Integer timer conversion shared with host tests. Whole seconds are
 * separated from the fractional remainder to avoid multiplying the complete
 * tick count by one billion.
 */
#include <ktime.h>

/**
 * ktime_ticks_to_ns() - Scale raw ticks without a full-width product
 * @ticks: Counter value or interval in ticks.
 * @frequency: Counter frequency in hertz; zero disables conversion.
 *
 * The remainder is converted separately from whole seconds. Callers using
 * absolute epochs must respect the uint64 nanosecond range.
 *
 * Context: Any context; no shared state, allocation, or sleeping.
 * Return: Nanoseconds rounded down; %0 for zero frequency. Whole-second
 *         overflow returns the largest uint64 value.
 */
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
