	.section .rodata
stack_message:
	.ascii "ELF_EXEC_STACK_OK\n"
	.equ STACK_MESSAGE_LENGTH, . - stack_message
stack_failure_message:
	.ascii "ELF_EXEC_STACK_FAIL\n"
	.equ STACK_FAILURE_MESSAGE_LENGTH, . - stack_failure_message
guard_failure_message:
	.ascii "ELF_GUARD_ACCESSIBLE\n"
	.equ GUARD_FAILURE_MESSAGE_LENGTH, . - guard_failure_message

	.section .text
	.globl _start
_start:
	addi sp, sp, -16
	li t0, 0x02a00513
	sw t0, 0(sp)
	li t0, 0x00008067
	sw t0, 4(sp)
	fence.i
	jalr ra, sp, 0
	li t0, 42
	bne a0, t0, .Lstack_failure

	li a0, 1
	lla a1, stack_message
	li a2, STACK_MESSAGE_LENGTH
	li a7, 64
	ecall

	lla t0, __guard_start
	ld t1, 0(t0)
	li a0, 1
	lla a1, guard_failure_message
	li a2, GUARD_FAILURE_MESSAGE_LENGTH
	li a7, 64
	ecall
	li a0, 1
	li a7, 93
	ecall

.Lstack_failure:
	li a0, 1
	lla a1, stack_failure_message
	li a2, STACK_FAILURE_MESSAGE_LENGTH
	li a7, 64
	ecall
	li a0, 1
	li a7, 93
	ecall
