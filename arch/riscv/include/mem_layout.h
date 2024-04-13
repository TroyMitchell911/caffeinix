#ifndef __CAFFEINIX_ARCH_RISCV_MEM_LAYOUT_H
#define __CAFFEINIX_ARCH_RISCV_MEM_LAYOUT_H

/* Only the address after 0x80000000L belongs DRAM */
#define KERNEL_BASE     0x80000000L
/* The DRAM memory of qemu have set 128M */
#define PHY_MEM_STOP    (KERNEL_BASE) + (128 * 1024 * 1024)

/* qemu puts UART registers here in physical memory. */
#define UART0 0x10000000L
#define UART0_IRQ 10

/* virtio mmio interface */
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1

/* core local interruptor (CLINT), which contains the timer. */
#define CLINT 0x2000000L
#define CLINT_MTIMECMP(hartid) (CLINT + 0x4000 + 8*(hartid))
/* cycles since boot. */
#define CLINT_MTIME (CLINT + 0xBFF8) 

#endif
