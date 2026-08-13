/*
 * @Author: TroyMitchell
 * @Date: 2024-04-26
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-16
 * @FilePath: /caffeinix/arch/riscv/boot/setup.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <riscv.h>
#include <boot.h>
#include <cpu.h>

extern void main(void);

/* OpenSBI starts only the boot hart before SBI HSM is used. */
__attribute__((aligned(16))) int8 boot_stack[BOOT_STACK_SIZE];
uint64 boot_dtb_address;
uint64 boot_hart_id;

void setup(uint64 hart_id, uint64 dtb_address)
{
	/* The standard next-stage contract enters with paging disabled. */
	satp_w(0);
	sfence_vma();
	boot_hart_id = hart_id;
	boot_dtb_address = dtb_address;
	/* Caffeinix uses tp as a dense logical CPU ID. */
	tp_w(0);
	main();
	for (;;)
		;
}

void secondary_setup(uint64 hart_id, uint64 stack_address)
{
	uint64 logical_id = tp_r();

	satp_w(0);
	sfence_vma();
	cpu_secondary_validate(hart_id, logical_id, stack_address);
	main();
	for (;;)
		;
}
