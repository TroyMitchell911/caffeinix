#ifndef __CAFFEINIX_ARCH_RISCV_CPU_H
#define __CAFFEINIX_ARCH_RISCV_CPU_H

#include <riscv.h>
#include <typedefs.h>

struct device_node;

void cpu_topology_init(uint64 boot_hart_id);
int cpu_count(void);
uint64 cpu_hart_id(int logical_id);
struct device_node *cpu_of_node(int logical_id);
void cpu_map_kernel_stacks(pagedir_t pgdir);
int cpu_kernel_stack_selftest(void);
uint64 cpu_scheduler_stack_top(void);
void cpu_enter_stack(uint64 stack_top, void (*entry)(void))
	__attribute__((noreturn));
void cpu_secondary_validate(uint64 hart_id, uint64 logical_id,
			    uint64 stack_address);
void cpu_secondary_boot_stack_release(void);
void cpu_start_secondary_harts(void);
void cpu_mark_online(void);
void cpu_membarrier_init(void);
void cpu_membarrier_interrupt(void);
void cpu_membarrier(void);
void cpu_icache_flush_all(void);
void cpu_tlb_flush_all(void);
void cpu_wait_for_secondary_harts(void);

#endif
