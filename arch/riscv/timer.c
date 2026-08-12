#include <debug.h>
#include <kernel_config.h>
#include <of.h>
#include <riscv.h>
#include <sbi.h>
#include <timer.h>

static uint64 clock_frequency;
static uint64 tick_interval;

void timer_init(void)
{
	struct device_node *cpus = of_find_node_by_path("/cpus");
	uint32 frequency;

	if (!cpus ||
	    of_property_read_u32(cpus, "timebase-frequency", &frequency) < 0 ||
	    !frequency)
		PANIC("missing timebase-frequency");
	clock_frequency = frequency;
	tick_interval = clock_frequency * TICK_INTERVAL / 1000;
	if (!tick_interval)
		PANIC("invalid timer interval");
}

void timer_init_hart(void)
{
	if (sbi_set_timer(time_r() + tick_interval))
		PANIC("SBI set_timer failed");
	sie_w(sie_r() | SIE_STIE);
}

void timer_interrupt(void)
{
	if (sbi_set_timer(time_r() + tick_interval))
		PANIC("SBI timer rearm failed");
}

uint64 timer_frequency(void)
{
	return clock_frequency;
}
