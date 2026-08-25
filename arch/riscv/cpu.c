#include <cpu.h>
#include <debug.h>
#include <kernel_config.h>
#include <mem_layout.h>
#include <of.h>
#include <palloc.h>
#include <printk.h>
#include <riscv.h>
#include <sbi.h>
#include <scheduler.h>
#include <sleeplock.h>
#include <timer.h>
#include <vm.h>

#define CPU_START_TIMEOUT_SECONDS 5

extern void secondary_entry(void);

static int logical_cpu_count;
static struct sleeplock membarrier_lock;
static uint64 membarrier_generation;

/* Used directly by the trap entry before it can safely use a C stack. */
uint64 *cpu_overflow_stack_tops;

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

void cpu_membarrier_init(void)
{
	sleeplock_init(&membarrier_lock, "CPU membarrier");
	membarrier_generation = 0;
}

void cpu_membarrier_interrupt(void)
{
	cpu_t cpu = cur_cpu();
	uint64 request;

	request = __atomic_load_n(&cpu->membarrier_request,
	                          __ATOMIC_ACQUIRE);
	if (request == __atomic_load_n(&cpu->membarrier_done,
	                              __ATOMIC_RELAXED))
		return;
	__sync_synchronize();
	__atomic_store_n(&cpu->membarrier_done, request, __ATOMIC_RELEASE);
}

void cpu_membarrier(void)
{
	uint64 generation;
	int current, logical;

	sleeplock_acquire(&membarrier_lock);
	generation = ++membarrier_generation;
	if (!generation)
		generation = ++membarrier_generation;
	current = cpuid();
	__sync_synchronize();
	for (logical = 0; logical < logical_cpu_count; logical++) {
		if (logical == current || !cpus[logical]->online)
			continue;
		__atomic_store_n(&cpus[logical]->membarrier_request,
		                 generation, __ATOMIC_RELEASE);
		if (sbi_send_ipi(cpu_hart_id(logical)))
			PANIC("membarrier IPI failed");
	}
	__sync_synchronize();
	for (logical = 0; logical < logical_cpu_count; logical++) {
		if (logical == current || !cpus[logical]->online)
			continue;
		while (__atomic_load_n(&cpus[logical]->membarrier_done,
		                       __ATOMIC_ACQUIRE) != generation)
			;
	}
	__sync_synchronize();
	sleeplock_release(&membarrier_lock);
}

void cpu_tlb_flush_all(void)
{
	int current = cpuid();
	int logical;

	sfence_vma();
	for (logical = 0; logical < logical_cpu_count; logical++) {
		if (logical == current || !cpus[logical]->online)
			continue;
		/* A zero range requests a complete address-space flush. */
		if (sbi_remote_sfence_vma(cpu_hart_id(logical), 0, 0))
			PANIC("remote TLB flush failed");
	}
}

void cpu_icache_flush_all(void)
{
	int current = cpuid();
	int logical;

	fence_i();
	for (logical = 0; logical < logical_cpu_count; logical++) {
		if (logical == current || !cpus[logical]->online)
			continue;
		if (sbi_remote_fence_i(cpu_hart_id(logical)))
			PANIC("remote instruction cache flush failed");
	}
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

static void cpu_map_kernel_stack(pagedir_t pgdir, int logical_id)
{
	uint64 address = SCHED_STACK(logical_id);
	uint64 physical;
	int page;

	if (cpus[logical_id]->scheduler_stack)
		PANIC("CPU stack already mapped");
	for (page = 0; page < KSTACK_PAGES; page++) {
		physical = (uint64)palloc();
		if (vm_map(pgdir, address + page * PGSIZE, physical,
		           PGSIZE, PTE_R | PTE_W) < 0)
			PANIC("map CPU stack");
	}
	cpus[logical_id]->scheduler_stack = (void *)address;
	physical = (uint64)palloc();
	cpu_overflow_stack_tops[logical_id] = physical + PGSIZE;
}

void cpu_map_kernel_stacks(pagedir_t pgdir)
{
	int logical;

	if (cpu_overflow_stack_tops)
		PANIC("CPU stacks already initialized");
	cpu_overflow_stack_tops =
		calloc(logical_cpu_count, sizeof(*cpu_overflow_stack_tops));
	if (!cpu_overflow_stack_tops)
		PANIC("allocate overflow stack table");
	for (logical = 0; logical < logical_cpu_count; logical++)
		cpu_map_kernel_stack(pgdir, logical);
}

static int cpu_stack_slot_valid(uint64 address)
{
	int page;

	if (address & (KSTACK_ALIGN - 1))
		return 0;
	if (kvm_va2pa(address - PGSIZE) ||
	    kvm_va2pa(address + KSTACK_SIZE))
		return 0;
	for (page = 0; page < KSTACK_PAGES; page++) {
		if (!kvm_va2pa(address + page * PGSIZE))
			return 0;
	}
	return 1;
}

int cpu_kernel_stack_selftest(void)
{
	uint64 address, overflow_top;
	int logical, thread_id;

	for (thread_id = 0; thread_id < NTHREAD; thread_id++) {
		if (!cpu_stack_slot_valid(KSTACK(thread_id)))
			return -1;
	}
	for (logical = 0; logical < logical_cpu_count; logical++) {
		address = (uint64)cpus[logical]->scheduler_stack;
		overflow_top = cpu_overflow_stack_tops[logical];
		if (address != SCHED_STACK(logical) ||
		    !cpu_stack_slot_valid(address) ||
		    !overflow_top || !kvm_va2pa(overflow_top - PGSIZE))
			return -1;
	}
	return 0;
}

uint64 cpu_scheduler_stack_top(void)
{
	uint64 stack = (uint64)cur_cpu()->scheduler_stack;

	if (!stack)
		PANIC("missing CPU stack");
	return stack + KSTACK_SIZE;
}

void cpu_secondary_validate(uint64 hart_id, uint64 logical_id,
			    uint64 stack_address)
{
	if (!logical_id || logical_id >= (uint64)logical_cpu_count ||
	    cpus[logical_id]->hart_id != hart_id ||
	    cpus[logical_id]->secondary_boot_stack !=
		(void *)stack_address)
		PANIC("invalid secondary hart handoff");
}

void cpu_secondary_boot_stack_release(void)
{
	cpu_t cpu = cur_cpu();
	void *stack = cpu->secondary_boot_stack;

	if (!stack)
		PANIC("missing secondary boot stack");
	cpu->secondary_boot_stack = 0;
	pfree(stack);
}

static void cpu_start_failed(int logical_id, int64 error)
{
	pr_err("CPU: logical=%d hart=%p SBI error=%d", logical_id,
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
			pr_err("CPU: logical=%d hart=%p HSM state=%d", logical,
			       cpu_hart_id(logical), (int)status);
			PANIC("secondary hart is not stopped");
		}
		if (cpus[logical]->secondary_boot_stack)
			PANIC("secondary boot stack already allocated");
		stack = palloc();
		cpus[logical]->secondary_boot_stack = stack;
		/* entry.S reads the logical CPU ID before using the stack. */
		*(uint64 *)stack = logical;
		__sync_synchronize();
		error = sbi_hart_start(cpu_hart_id(logical),
		                       (uint64)secondary_entry,
		                       (uint64)stack);
		if (error) {
			cpus[logical]->secondary_boot_stack = 0;
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
	pr_info("CPU: logical=%d hart=%p online", logical,
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
		pr_err("CPU: logical=%d hart=%p online timeout", logical,
		       cpu_hart_id(logical));
		PANIC("secondary hart online timeout");
	}
}
