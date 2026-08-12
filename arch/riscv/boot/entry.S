.section .text
.global _entry

_entry:
	# OpenSBI passes the boot hart ID in a0 and the DTB address in a1.
	la sp, boot_stack
	li t0, 4096
	add sp, sp, t0
	tail setup
