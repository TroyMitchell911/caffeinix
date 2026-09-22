/*
 * Kernel time domains derived from the RISC-V time counter.
 *
 * Raw monotonic time uses the counter epoch; boot time subtracts the entry
 * timestamp. Realtime adds elapsed counter time to the boot RTC sample (or
 * the explicit zero-epoch fallback). Realtime writers run during serialized
 * boot, before concurrent readers.
 */
#include <ktime.h>
#include <riscv.h>
#include <timer.h>

#define MSEC_PER_SEC 1000ULL

static uint64 boot_ticks;
static uint64 realtime_base_ns;
static uint64 realtime_base_ticks;
static uint8 realtime_ready;

/**
 * ktime_boot_init() - Record the clock epoch at kernel entry
 * @ticks: Raw time-counter sample taken at boot entry.
 *
 * Also establishes a zero realtime epoch. This fallback is not a claim that
 * the hardware clock contains valid civil time.
 *
 * Context: Boot hart only, before SMP startup; does not sleep.
 */
void ktime_boot_init(uint64 ticks)
{
	boot_ticks = ticks;
	realtime_base_ns = 0;
	realtime_base_ticks = ticks;
	__atomic_store_n(&realtime_ready, 1, __ATOMIC_RELEASE);
}

/**
 * ktime_set_realtime_ns() - Publish a wall-clock sample during boot
 * @nanoseconds: Nanoseconds since the Unix epoch from the boot RTC.
 *
 * Subsequent realtime reads add elapsed hardware-counter time to this sample.
 *
 * Context: Serialized boot initialization; not a concurrent clock-setting
 *          API.
 */
void ktime_set_realtime_ns(uint64 nanoseconds)
{
	realtime_base_ns = nanoseconds;
	realtime_base_ticks = time_r();
	__atomic_store_n(&realtime_ready, 1, __ATOMIC_RELEASE);
}

/**
 * ktime_get_ticks() - Read the hardware time counter
 *
 * Context: Supervisor context; does not sleep or acquire locks.
 * Return: Raw counter ticks, not scheduler ticks or nanoseconds.
 */
uint64 ktime_get_ticks(void)
{
	return time_r();
}

/**
 * ktime_get_ms() - Convert the counter epoch to milliseconds
 *
 * Context: After timer-frequency discovery; does not sleep.
 * Return: Elapsed milliseconds from the hardware counter epoch, rounded down.
 */
uint64 ktime_get_ms(void)
{
	uint64 ticks = time_r();
	uint64 frequency = timer_frequency();

	return ticks / frequency * MSEC_PER_SEC +
	       ticks % frequency * MSEC_PER_SEC / frequency;
}

/**
 * ktime_get_ns() - Convert the counter epoch to nanoseconds
 *
 * Context: After timer-frequency discovery; does not sleep.
 * Return: Counter-epoch nanoseconds using ktime_ticks_to_ns().
 */
uint64 ktime_get_ns(void)
{
	return ktime_ticks_to_ns(time_r(), timer_frequency());
}

/**
 * ktime_get_boot_ns() - Read elapsed time since kernel entry
 *
 * Context: After clock initialization; does not sleep.
 * Return: Boot-relative nanoseconds, not a wall-clock timestamp.
 */
uint64 ktime_get_boot_ns(void)
{
	return ktime_ticks_to_ns(time_r() - boot_ticks,
				 timer_frequency());
}

/**
 * ktime_get_realtime_ns() - Read the published realtime clock
 * @nanoseconds: Non-NULL kernel pointer receiving epoch nanoseconds.
 *
 * The addition of elapsed time saturates at the largest uint64 value. A boot
 * without an RTC uses the zero-epoch fallback.
 *
 * Context: After serialized boot publication; does not sleep.
 * Return: %0 on success, %-1 for a NULL output or an unpublished epoch.
 */
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

/**
 * ktime_ms_to_ticks() - Round a relative millisecond interval upward
 * @milliseconds: Relative duration in milliseconds.
 *
 * Rounding upward prevents sub-tick nonzero waits from expiring immediately.
 *
 * Context: After timer-frequency discovery; does not sleep.
 * Return: Counter ticks rounded upward, saturated on overflow.
 */
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

/**
 * ktime_ns_to_ticks() - Round a relative nanosecond interval upward
 * @nanoseconds: Relative duration in nanoseconds.
 *
 * Context: After timer-frequency discovery; does not sleep.
 * Return: Counter ticks rounded upward, saturated on overflow.
 */
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
