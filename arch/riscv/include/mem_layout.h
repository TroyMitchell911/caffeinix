#ifndef __CAFFEINIX_ARCH_RISCV_MEM_LAYOUT_H
#define __CAFFEINIX_ARCH_RISCV_MEM_LAYOUT_H

/* Only the address after 0x80000000L belongs DRAM */
#define KERNEL_BASE     0x80000000L
/* The DRAM memory of qemu have set 128M */
#define PHY_MEM_STOP    (KERNEL_BASE) + (128 * 1024 * 1024)
/* 
        map the trampoline page to the highest address,
        in both user and kernel space.
 */
#define TRAMPOLINE (MAXVA - PGSIZE)
#define TRAPFRAME (TRAMPOLINE - PGSIZE)
/* 
        map kernel stacks beneath the trampoline,
        each surrounded by invalid guard pages.
 */
#define KSTACK(p) (TRAMPOLINE - ((p)+1)* 2*PGSIZE)

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

/* qemu puts platform-level interrupt controller (PLIC) here. */
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_MENABLE(hart) (PLIC + 0x2000 + (hart)*0x100)
#define PLIC_SENABLE(hart) (PLIC + 0x2080 + (hart)*0x100)
#define PLIC_MPRIORITY(hart) (PLIC + 0x200000 + (hart)*0x2000)
#define PLIC_SPRIORITY(hart) (PLIC + 0x201000 + (hart)*0x2000)
#define PLIC_MCLAIM(hart) (PLIC + 0x200004 + (hart)*0x2000)
#define PLIC_SCLAIM(hart) (PLIC + 0x201004 + (hart)*0x2000)

#endif
