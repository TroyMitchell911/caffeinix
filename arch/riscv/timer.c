#include <cpu.h>
#include <debug.h>
#include <kernel_config.h>
#include <of.h>
#include <palloc.h>
#include <printf.h>
#include <riscv.h>
#include <sbi.h>
#include <scheduler.h>
#include <timer.h>

static uint64 clock_frequency;
static uint64 tick_interval;
static uint64 idle_tick_interval;

struct timer_cpu_state {
	volatile uint8 seen;
	volatile uint8 idle;
};

static struct timer_cpu_state *timer_cpus;

void timer_early_init(void)
{
	struct device_node *cpus = of_find_node_by_path("/cpus");
	uint32 frequency;

	if (!cpus ||
	    of_property_read_u32(cpus, "timebase-frequency", &frequency) < 0 ||
	    !frequency)
		PANIC("missing timebase-frequency");
	clock_frequency = frequency;
	tick_interval = clock_frequency * TICK_INTERVAL / 1000;
	idle_tick_interval = clock_frequency * IDLE_TICK_INTERVAL / 1000;
	if (!tick_interval || !idle_tick_interval)
		PANIC("invalid timer interval");
}

void timer_init(void)
{
	if (!clock_frequency)
		PANIC("timer clocksource is not initialized");
	timer_cpus = calloc(cpu_count(), sizeof(*timer_cpus));
	if (!timer_cpus)
		PANIC("allocate timer CPU state");
}

void timer_init_hart(void)
{
	if (sbi_set_timer(time_r() + tick_interval))
		PANIC("SBI set_timer failed");
	sie_w(sie_r() | SIE_STIE);
}

void timer_wait_for_interrupt(void)
{
	uint64 start = time_r();
	int logical = cpuid();

	if (logical < 0 || logical >= cpu_count())
		PANIC("invalid timer CPU");
	intr_on();
	while (!timer_cpus[logical].seen) {
		if (time_r() - start > clock_frequency * 2) {
			intr_off();
			PANIC("supervisor timer timeout");
		}
	}
	intr_off();
}

void timer_interrupt(void)
{
	int logical = cpuid();
	uint64 interval;

	if (logical < 0 || logical >= cpu_count())
		PANIC("invalid timer CPU");
	interval = timer_cpus[logical].idle ? idle_tick_interval :
		   tick_interval;
	if (sbi_set_timer(time_r() + interval))
		PANIC("SBI timer rearm failed");
	if (!timer_cpus[logical].seen) {
		timer_cpus[logical].seen = 1;
		printf("CPU: logical=%d timer active\n", logical);
	}
}

void timer_set_active(void)
{
	int logical = cpuid();

	if (logical < 0 || logical >= cpu_count())
		PANIC("invalid timer CPU");
	if (!__atomic_exchange_n(&timer_cpus[logical].idle, 0,
				 __ATOMIC_ACQ_REL))
		return;
	if (sbi_set_timer(time_r() + tick_interval))
		PANIC("SBI active timer failed");
}

void timer_set_idle(void)
{
	int logical = cpuid();

	if (logical < 0 || logical >= cpu_count())
		PANIC("invalid timer CPU");
	/* CPU0 retains the fine tick that expires global timed waits. */
	if (!logical || __atomic_exchange_n(&timer_cpus[logical].idle, 1,
					    __ATOMIC_ACQ_REL))
		return;
	if (sbi_set_timer(time_r() + idle_tick_interval))
		PANIC("SBI idle timer failed");
}

uint64 timer_frequency(void)
{
	return clock_frequency;
}
