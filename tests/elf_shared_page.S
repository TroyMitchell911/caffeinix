	.section .rodata
message:
	.ascii "ELF_SHARED_PAGE_OK\n"
	.equ MESSAGE_LENGTH, . - message

	.section .data
	.globl shared_data
shared_data:
	.quad 0x4341464645494e49

	.section .text
	.globl _start
_start:
	la t0, shared_data
	ld t1, 0(t0)
	addi t1, t1, 1
	sd t1, 0(t0)
	li a0, 1
	la a1, message
	li a2, MESSAGE_LENGTH
	li a7, 64
	ecall
	li a0, 0
	li a7, 93
	ecall
