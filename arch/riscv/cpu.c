#include <cpu.h>
#include <debug.h>
#include <of.h>
#include <palloc.h>
#include <printf.h>
#include <riscv.h>
#include <sbi.h>
#include <scheduler.h>
#include <timer.h>

#define CPU_START_TIMEOUT_SECONDS 5

extern void secondary_entry(void);

static int logical_cpu_count;

void cpu_topology_init(uint64 boot_hart_id)
{
	struct device_node *node;
	cpu_t *table;
	uint64 hart_id;
	int boot_count = 0, count, index, logical, logical_count = 1;

	count = of_cpu_count();
	if (count < 1)
		PANIC("unsupported CPU count");
	table = calloc(count, sizeof(*table));
	if (!table)
		PANIC("allocate CPU table");
	table[0] = cpus[0];
	for (logical = 1; logical < count; logical++) {
		table[logical] = calloc(1, sizeof(*table[logical]));
		if (!table[logical])
			PANIC("allocate CPU state");
	}
	table[0]->hart_id = boot_hart_id;
	for (index = 0; index < count; index++) {
		if (of_cpu_get(index, &node, &hart_id) < 0)
			PANIC("invalid CPU node");
		if (hart_id == boot_hart_id) {
			table[0]->of_node = node;
			boot_count++;
			continue;
		}
		for (logical = 1; logical < logical_count; logical++) {
			if (table[logical]->hart_id == hart_id)
				PANIC("duplicate hart ID");
		}
		if (logical_count >= count)
			PANIC("boot hart missing from DT");
		logical = logical_count++;
		table[logical]->hart_id = hart_id;
		table[logical]->of_node = node;
	}
	if (boot_count != 1 || logical_count != count)
		PANIC("boot hart missing from DT");
	cpus = table;
	logical_cpu_count = count;
}

int cpu_count(void)
{
	return logical_cpu_count;
}

uint64 cpu_hart_id(int logical_id)
{
	if (logical_id < 0 || logical_id >= logical_cpu_count)
		PANIC("invalid logical CPU");
	return cpus[logical_id]->hart_id;
}

struct device_node *cpu_of_node(int logical_id)
{
	if (logical_id < 0 || logical_id >= logical_cpu_count)
		return 0;
	return cpus[logical_id]->of_node;
}

void cpu_secondary_validate(uint64 hart_id, uint64 logical_id,
			    uint64 stack_address)
{
	if (!logical_id || logical_id >= (uint64)logical_cpu_count ||
	    cpus[logical_id]->hart_id != hart_id ||
	    cpus[logical_id]->scheduler_stack != (void *)stack_address)
		PANIC("invalid secondary hart handoff");
}

static void cpu_start_failed(int logical_id, int64 error)
{
	printf("CPU: logical=%d hart=%p SBI error=%d\n", logical_id,
	       cpu_hart_id(logical_id), (int)error);
	PANIC("secondary hart start failed");
}

void cpu_start_secondary_harts(void)
{
	uint64 status;
	int64 error;
	int logical;

	for (logical = 1; logical < logical_cpu_count; logical++) {
		void *stack;

		error = sbi_hart_get_status(cpu_hart_id(logical), &status);
		if (error)
			cpu_start_failed(logical, error);
		if (status != SBI_HSM_STATE_STOPPED) {
			printf("CPU: logical=%d hart=%p HSM state=%d\n", logical,
			       cpu_hart_id(logical), (int)status);
			PANIC("secondary hart is not stopped");
		}
		if (cpus[logical]->scheduler_stack)
			PANIC("secondary hart stack already allocated");
		stack = palloc();
		cpus[logical]->scheduler_stack = stack;
		/* entry.S reads the logical CPU ID before using the stack. */
		*(uint64 *)stack = logical;
		__sync_synchronize();
		error = sbi_hart_start(cpu_hart_id(logical),
		                       (uint64)secondary_entry,
		                       (uint64)stack);
		if (error) {
			cpus[logical]->scheduler_stack = 0;
			pfree(stack);
			cpu_start_failed(logical, error);
		}
	}
}

void cpu_mark_online(void)
{
	int logical = cpuid();

	if (logical < 0 || logical >= logical_cpu_count ||
	    cpus[logical]->online)
		PANIC("duplicate CPU online");
	__sync_synchronize();
	cpus[logical]->online = 1;
	__sync_synchronize();
	printf("CPU: logical=%d hart=%p online\n", logical,
	       cpus[logical]->hart_id);
}

void cpu_wait_for_secondary_harts(void)
{
	uint64 start = time_r();
	uint64 timeout = timer_frequency() * CPU_START_TIMEOUT_SECONDS;
	int logical;

	for (;;) {
		for (logical = 1; logical < logical_cpu_count; logical++) {
			if (!cpus[logical]->online)
				break;
		}
		if (logical == logical_cpu_count)
			return;
		if (time_r() - start < timeout)
			continue;
		printf("CPU: logical=%d hart=%p online timeout\n", logical,
		       cpu_hart_id(logical));
		PANIC("secondary hart online timeout");
	}
}
