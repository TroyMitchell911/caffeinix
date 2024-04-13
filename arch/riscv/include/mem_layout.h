#ifndef __CAFFEINIX_ARCH_RISCV_MEM_LAYOUT_H
#define __CAFFEINIX_ARCH_RISCV_MEM_LAYOUT_H

/* Only the address after 0x80000000L belongs DRAM */
#define KERNEL_BASE     0x80000000L
/* The DRAM memory of qemu have set 128M */
#define PHY_MEM_STOP    (KERNEL_BASE) + (128 * 1024 * 1024)

#endif
