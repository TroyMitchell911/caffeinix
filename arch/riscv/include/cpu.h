#ifndef __CAFFEINIX_ARCH_RISCV_CPU_H
#define __CAFFEINIX_ARCH_RISCV_CPU_H

#include <typedefs.h>

struct device_node;

void cpu_topology_init(uint64 boot_hart_id);
int cpu_count(void);
uint64 cpu_hart_id(int logical_id);
struct device_node *cpu_of_node(int logical_id);
void cpu_secondary_validate(uint64 hart_id, uint64 logical_id);
void cpu_start_secondary_harts(void);
void cpu_mark_online(void);
void cpu_wait_for_secondary_harts(void);

#endif
