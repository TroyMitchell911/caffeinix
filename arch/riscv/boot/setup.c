/*
 * C entry points reached from firmware and SBI HSM start stubs.
 * Both paths begin with paging disabled; boot uses tp=0 and secondaries
 * receive their dense logical CPU ID in tp from entry.S.
 *
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved.
 */
#include <riscv.h>
#include <boot.h>
#include <cpu.h>
#include <ktime.h>

extern void main(void);

/* OpenSBI starts only the boot hart before SBI HSM is used. */
__attribute__((aligned(16))) int8 boot_stack[BOOT_STACK_SIZE];
uint64 boot_dtb_address;
uint64 boot_hart_id;

/**
 * setup() - Enter kernel initialization from the firmware boot hart.
 * @hart_id: Firmware hart ID of the boot hart.
 * @dtb_address: Physical address of the firmware device tree.
 *
 * Context:
 * Paging-disabled early S-mode entry; does not return or sleep.
 */
void setup(uint64 hart_id, uint64 dtb_address)
{
	/* The standard next-stage contract enters with paging disabled. */
	satp_w(0);
	sfence_vma();
	ktime_boot_init(time_r());
	boot_hart_id = hart_id;
	boot_dtb_address = dtb_address;
	/* Caffeinix uses tp as a dense logical CPU ID. */
	tp_w(0);
	main();
	for (;;)
		;
}

/**
 * secondary_setup() - Enter common initialization on an HSM-started hart.
 * @hart_id: Firmware hart ID supplied by HSM.
 * @stack_address: Physical temporary boot-stack address supplied to HSM.
 *
 * Context:
 * Paging-disabled early S-mode entry; validates handoff and does not
 * return or sleep.
 */
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
