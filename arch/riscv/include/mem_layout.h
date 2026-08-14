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

#include <kernel_config.h>

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
 * Put each runtime kernel stack in the lower half of an aligned slot.
 * The unused upper half and the preceding slot's upper half guard both
 * ends, while KSTACK_SHIFT identifies an overflow at trap entry.
 */
#define KSTACK_SHIFT 14
#define KSTACK_SIZE (1 << KSTACK_SHIFT)
#define KSTACK_PAGES (KSTACK_SIZE / PGSIZE)
#define KSTACK_ALIGN (2 * KSTACK_SIZE)
#define KSTACK_REGION_TOP (TRAMPOLINE & ~(KSTACK_ALIGN - 1))
#define KSTACK(p) \
	(KSTACK_REGION_TOP - ((p) + 1) * KSTACK_ALIGN)
#define SCHED_STACK(cpu) \
	(KSTACK_REGION_TOP - (NTHREAD + (cpu) + 1) * KSTACK_ALIGN)

/* qemu puts platform-level interrupt controller (PLIC) here. */
#define PLIC 0x0c000000L
#define PLIC_PRIORITY (PLIC + 0x0)
#define PLIC_PENDING (PLIC + 0x1000)
#define PLIC_ENABLE(context) (PLIC + 0x2000 + (context) * 0x80)
#define PLIC_THRESHOLD(context) (PLIC + 0x200000 + (context) * 0x1000)
#define PLIC_CLAIM(context) (PLIC_THRESHOLD(context) + 4)

#endif
