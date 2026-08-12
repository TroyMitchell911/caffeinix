#include <kernel_config.h>

.section .text
.global _entry
.global secondary_entry

_entry:
	# OpenSBI passes the boot hart ID in a0 and the DTB address in a1.
	la sp, boot_stack
	li t0, 4096
	add sp, sp, t0
	tail setup

secondary_entry:
	li t0, NCPU
	bgeu a1, t0, .Lsecondary_park
	la sp, secondary_stacks
	slli t0, a1, 12
	add sp, sp, t0
	li t0, 4096
	add sp, sp, t0
	mv tp, a1
	tail secondary_setup

.Lsecondary_park:
	wfi
	j .Lsecondary_park
