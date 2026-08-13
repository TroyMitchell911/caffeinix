#include <boot.h>

.section .text
.global _entry
.global secondary_entry

_entry:
	# OpenSBI passes the boot hart ID in a0 and the DTB address in a1.
	la sp, boot_stack
	li t0, BOOT_STACK_SIZE
	add sp, sp, t0
	tail setup

secondary_entry:
	# The HSM opaque value points at this hart's scheduler stack.
	mv t0, a1
	ld tp, 0(t0)
	li t1, PGSIZE
	add sp, t0, t1
	tail secondary_setup
