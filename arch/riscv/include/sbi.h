#ifndef __CAFFEINIX_ARCH_RISCV_SBI_H
#define __CAFFEINIX_ARCH_RISCV_SBI_H

#include <typedefs.h>

#define SBI_HSM_STATE_STARTED 0
#define SBI_HSM_STATE_STOPPED 1

void sbi_init(int requested_cpus);
void sbi_report(void);
int64 sbi_set_timer(uint64 deadline);
int64 sbi_hart_start(uint64 hart_id, uint64 start_address,
		     uint64 opaque);
int64 sbi_hart_get_status(uint64 hart_id, uint64 *status);
int64 sbi_send_ipi(uint64 hart_id);
int64 sbi_remote_fence_i(uint64 hart_id);
int64 sbi_remote_sfence_vma(uint64 hart_id, uint64 start, uint64 size);

#endif
