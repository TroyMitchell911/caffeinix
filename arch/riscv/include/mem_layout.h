/*
 * @Author: TroyMitchell
 * @Date: 2024-04-18
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-15
 * @FilePath: /caffeinix/arch/riscv/include/mem_layout.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_ARCH_RISCV_MEM_LAYOUT_H
#define __CAFFEINIX_ARCH_RISCV_MEM_LAYOUT_H

/* OpenSBI remains resident at the start of RAM on QEMU virt. */
#define KERNEL_BASE     0x80200000L
/* 
        map the trampoline page to the highest address,
        in both user and kernel space.
 */
#define TRAMPOLINE      (MAXVA - PGSIZE)
#define TRAPFRAME_INFO   (TRAMPOLINE - PGSIZE)
#define TRAPFRAME(x)    (TRAPFRAME_INFO - ((PGSIZE) * (x + 1)))

/* Keep the userspace stack separate from the ELF break and mmap area. */
#define USER_STACK_TOP   0x40000000L
#define USER_STACK_SIZE  (64 * PGSIZE)
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_SIZE)
#define USER_MMAP_TOP    (USER_STACK_BASE - PGSIZE)
/* 
        map kernel stacks beneath the trampoline,
        each surrounded by invalid guard pages.
 */
#define KSTACK_PAGES 4
#define KSTACK_SIZE (KSTACK_PAGES * PGSIZE)
#define KSTACK(p) \
	(TRAMPOLINE - ((p) + 1) * (KSTACK_PAGES + 1) * PGSIZE)

/* virtio mmio interface */
#define VIRTIO0 0x10001000
#define VIRTIO0_IRQ 1
#define VIRTIO_MMIO_SLOTS 8

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
