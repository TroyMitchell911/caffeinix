	.section .interp, "a", @progbits
#ifdef ELF_FIXED_INTERPRETER
	.asciz "/lib/ld-caffeinix-riscv64.so.1"
#else
	.asciz "/lib/ld-musl-riscv64.so.1"
#endif

	.section .rodata
main_message:
#ifdef ELF_FIXED_INTERPRETER
	.ascii "ELF_FIXED_MAIN_ENTRY_OK\n"
#else
	.ascii "ELF_MAIN_ENTRY_OK\n"
#endif
	.equ MAIN_MESSAGE_LENGTH, . - main_message

	.section .text
	.globl _start
_start:
	li a0, 1
	lla a1, main_message
	li a2, MAIN_MESSAGE_LENGTH
	li a7, 64
	ecall
	li a0, 0
	li a7, 93
	ecall
