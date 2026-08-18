	.section .rodata
unexpected_message:
	.ascii "ELF_MISSING_INTERP_STARTED\n"
	.equ UNEXPECTED_MESSAGE_LENGTH, . - unexpected_message

	.section .interp, "a", @progbits
	.asciz "/lib/missing-stage2"

	.section .text
	.globl _start
_start:
	li a0, 1
	lla a1, unexpected_message
	li a2, UNEXPECTED_MESSAGE_LENGTH
	li a7, 64
	ecall
	li a0, 1
	li a7, 93
	ecall
