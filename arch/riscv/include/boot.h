#ifndef __CAFFEINIX_ARCH_RISCV_BOOT_H
#define __CAFFEINIX_ARCH_RISCV_BOOT_H

#include <riscv.h>

#define BOOT_STACK_PAGES 4
#define BOOT_STACK_SIZE (BOOT_STACK_PAGES * PGSIZE)

#ifndef __ASSEMBLER__
#include <typedefs.h>

extern uint64 boot_dtb_address;
extern uint64 boot_hart_id;
#endif

#endif
