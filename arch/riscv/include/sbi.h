/* OpenSBI v0.2+ calls required by the S-mode kernel. */
#ifndef __CAFFEINIX_ARCH_RISCV_SBI_H
#define __CAFFEINIX_ARCH_RISCV_SBI_H

#include <typedefs.h>

#define SBI_HSM_STATE_STARTED 0
#define SBI_HSM_STATE_STOPPED 1

/**
 * sbi_init() - Probe required SBI extensions.
 * @requested_cpus: Number of enabled logical CPUs.
 *
 * Context:
 * Early boot after console setup. Panics when firmware lacks a
 * required extension.
 */
void sbi_init(int requested_cpus);
/**
 * sbi_report() - Print the SBI specification and implementation identity.
 *
 * Context:
 * After sbi_init() and console setup; does not sleep.
 */
void sbi_report(void);
/**
 * sbi_set_timer() - Program an absolute SBI TIME deadline.
 * @deadline: Timebase tick at which supervisor timer delivery is requested.
 *
 * Context:
 * Any S-mode context.
 * Return:
 * SBI error code, zero on success.
 */
int64 sbi_set_timer(uint64 deadline);
/**
 * sbi_hart_start() - Request HSM start of a stopped firmware hart.
 * @hart_id: Opaque firmware hart ID.
 * @start_address: Physical supervisor entry address accepted by firmware.
 * @opaque: Value delivered as a1 to that entry.
 *
 * Context:
 * Any S-mode context; does not sleep.
 *
 * Return:
 * SBI error domain value; zero means firmware accepted the request.
 */
int64 sbi_hart_start(uint64 hart_id, uint64 start_address,
		     uint64 opaque);
/**
 * sbi_hart_get_status() - Query one hart's SBI HSM state.
 * @hart_id: Opaque firmware hart ID.
 * @status: Non-NULL destination for the SBI state value.
 *
 * Context:
 * Any S-mode context; does not sleep.
 *
 * Return:
 * SBI error value, or -3 when @status is %NULL; @status changes only on zero.
 */
int64 sbi_hart_get_status(uint64 hart_id, uint64 *status);
/**
 * sbi_send_ipi() - Request an IPI for exactly one firmware hart.
 * @hart_id: Opaque target hart ID, including sparse IDs.
 *
 * Context:
 * Any S-mode context; does not sleep.
 *
 * Return:
 * SBI error domain value; zero means accepted.
 */
int64 sbi_send_ipi(uint64 hart_id);
/**
 * sbi_remote_fence_i() - Request remote instruction-fetch synchronization.
 * @hart_id: Opaque target hart ID.
 *
 * Context:
 * Any S-mode context after code writes are globally visible; does not sleep.
 *
 * Return:
 * SBI error domain value; zero means accepted.
 */
int64 sbi_remote_fence_i(uint64 hart_id);
/**
 * sbi_remote_sfence_vma() - Request a remote translation fence.
 * @hart_id: Opaque target hart ID.
 * @start: Virtual range start; zero requests all addresses.
 * @size: Byte range length; zero requests all addresses.
 *
 * Context:
 * Any S-mode context after PTE stores are visible; does not sleep.
 *
 * Return:
 * SBI error domain value; zero means accepted.
 */
int64 sbi_remote_sfence_vma(uint64 hart_id, uint64 start, uint64 size);

#endif
