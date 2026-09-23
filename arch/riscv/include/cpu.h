/* Logical-CPU topology, cross-hart cache maintenance, and stack lifecycle. */
#ifndef __CAFFEINIX_ARCH_RISCV_CPU_H
#define __CAFFEINIX_ARCH_RISCV_CPU_H

#include <riscv.h>
#include <typedefs.h>

struct device_node;

/**
 * cpu_topology_init() - Build logical CPU topology from the device tree.
 * @boot_hart_id: Firmware physical ID of the hart entering first.
 *
 * Context:
 * Single-threaded early boot; may allocate. The resulting logical
 * IDs are dense and physical hart IDs remain opaque firmware values.
 */
void cpu_topology_init(uint64 boot_hart_id);
/**
 * cpu_count() - Return the number of discovered logical CPUs.
 *
 * Context:
 * After cpu_topology_init(); does not sleep.
 * Return:
 * Dense logical CPU count.
 */
int cpu_count(void);
/**
 * cpu_hart_id() - Translate a logical CPU ID to a firmware hart ID.
 * @logical_id: Dense logical CPU index.
 *
 * Context:
 * After cpu_topology_init(); does not sleep.
 * Return:
 * Firmware hart ID. Invalid indices panic.
 */
uint64 cpu_hart_id(int logical_id);
/**
 * cpu_of_node() - Return the FDT node recorded for one logical CPU.
 * @logical_id: Dense index in the topology built by cpu_topology_init().
 *
 * Context:
 * Any context after topology initialization; does not sleep.
 *
 * Return:
 * Borrowed immutable FDT node, or %NULL for an invalid logical ID.
 */
struct device_node *cpu_of_node(int logical_id);
/**
 * cpu_map_kernel_stacks() - Allocate and map guarded scheduler stacks.
 * @pgdir: Unpublished kernel page directory.
 *
 * Context:
 * Early boot; may allocate. Runs once before secondary start.
 */
void cpu_map_kernel_stacks(pagedir_t pgdir);
/**
 * cpu_kernel_stack_selftest() - Check scheduler stack mappings and guards.
 *
 * Context:
 * After the kernel mapping and CPU stacks are stable; does not sleep.
 *
 * Return:
 * Zero when every thread and CPU stack has both guards, otherwise -1.
 */
int cpu_kernel_stack_selftest(void);
/**
 * cpu_scheduler_stack_top() - Return the current CPU scheduler stack top.
 *
 * Context:
 * Local CPU context after cpu_map_kernel_stacks(); does not sleep.
 *
 * Return:
 * Virtual address one byte beyond the stack; panics if absent.
 */
uint64 cpu_scheduler_stack_top(void);
/**
 * cpu_enter_stack() - Switch stacks and tail-call an entry function.
 * @stack_top: One byte beyond a mapped scheduler stack.
 * @entry: Non-returning C entry function.
 *
 * Context:
 * Local S-mode with a valid target stack. Does not return.
 */
void cpu_enter_stack(uint64 stack_top, void (*entry)(void))
	__attribute__((noreturn));
/**
 * cpu_secondary_validate() - Validate an SBI HSM secondary-hart handoff.
 * @hart_id: Firmware hart ID passed to the secondary entry.
 * @logical_id: Dense ID stored in tp by the assembly entry stub.
 * @stack_address: Physical temporary boot-stack page supplied as SBI opaque.
 *
 * Context:
 * Paging-disabled secondary early boot; does not sleep and panics on mismatch.
 */
void cpu_secondary_validate(uint64 hart_id, uint64 logical_id,
			    uint64 stack_address);
/**
 * cpu_secondary_boot_stack_release() - Free this secondary's temporary stack.
 *
 * Context:
 * Secondary hart after switching to its scheduler stack; does not sleep and
 * panics if no temporary stack was recorded.
 */
void cpu_secondary_boot_stack_release(void);
/**
 * cpu_start_secondary_harts() - Allocate handoffs and start all non-boot CPUs.
 *
 * Context:
 * Boot hart before scheduler concurrency; allocates pages and panics on SBI
 * failure.  Secondary stacks remain owned until their local release.
 */
void cpu_start_secondary_harts(void);
/**
 * cpu_mark_online() - Publish the current CPU as able to receive IPIs.
 *
 * Context:
 * Per-hart initialization after traps and timer setup; does not sleep.
 */
void cpu_mark_online(void);
/**
 * cpu_membarrier_init() - Initialize cross-hart barrier serialization.
 *
 * Context:
 * Single-threaded boot before cpu_membarrier(); does not sleep.
 */
void cpu_membarrier_init(void);
/**
 * cpu_membarrier_interrupt() - Acknowledge the local membarrier IPI.
 *
 * Context:
 * Supervisor software-interrupt context; lockless and non-sleeping.
 */
void cpu_membarrier_interrupt(void);
/**
 * cpu_membarrier() - Complete a full barrier on every online CPU.
 *
 * Context:
 * Thread context; serializes requests and waits for IPI acknowledgments.
 */
void cpu_membarrier(void);
/**
 * cpu_icache_flush_all() - Synchronize instruction caches on online CPUs.
 *
 * Context:
 * Caller has made code writes globally visible; does not sleep and panics on
 * SBI remote-fence failure.
 */
void cpu_icache_flush_all(void);
/**
 * cpu_tlb_flush_all() - Flush translations on every online CPU.
 *
 * Context:
 * Thread context; caller has completed PTE stores and may issue SBI
 * RFENCE calls. It waits only for firmware call completion.
 */
void cpu_tlb_flush_all(void);
/**
 * cpu_wait_for_secondary_harts() - Wait for all requested CPUs to come online
 *
 * Context:
 * Boot hart after cpu_start_secondary_harts(); does not sleep and panics after
 * the bounded timebase timeout.
 */
void cpu_wait_for_secondary_harts(void);

#endif
