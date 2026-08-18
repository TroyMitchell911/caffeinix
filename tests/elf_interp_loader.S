	.section .rodata
interpreter_message:
#ifdef ELF_FIXED_INTERPRETER
	.ascii "ELF_FIXED_INTERP_OK\n"
#else
	.ascii "ELF_INTERP_OK\n"
#endif
	.equ INTERPRETER_MESSAGE_LENGTH, . - interpreter_message
failure_message:
	.ascii "ELF_INTERP_FAIL\n"
	.equ FAILURE_MESSAGE_LENGTH, . - failure_message

	.section .text
	.globl _start
_start:
	mv t0, sp
	ld t1, 0(t0)
	addi t0, t0, 8
	slli t2, t1, 3
	add t0, t0, t2
	addi t0, t0, 8

.Lskip_environment:
	ld t1, 0(t0)
	addi t0, t0, 8
	bnez t1, .Lskip_environment

	li s0, 0
	li s1, 0
	li s2, 0
	li s3, 0
	li s4, 0

.Lauxiliary_vector:
	ld t1, 0(t0)
	ld t2, 8(t0)
	addi t0, t0, 16
	beqz t1, .Lvalidate
	li t3, 3
	beq t1, t3, .Lsave_phdr
	li t3, 4
	beq t1, t3, .Lsave_phent
	li t3, 5
	beq t1, t3, .Lsave_phnum
	li t3, 7
	beq t1, t3, .Lsave_base
	li t3, 9
	beq t1, t3, .Lsave_entry
	j .Lauxiliary_vector

.Lsave_phdr:
	mv s0, t2
	j .Lauxiliary_vector
.Lsave_phent:
	mv s1, t2
	j .Lauxiliary_vector
.Lsave_phnum:
	mv s2, t2
	j .Lauxiliary_vector
.Lsave_base:
	mv s3, t2
	j .Lauxiliary_vector
.Lsave_entry:
	mv s4, t2
	j .Lauxiliary_vector

.Lvalidate:
	beqz s0, .Lfailure
#ifdef ELF_FIXED_INTERPRETER
	bnez s3, .Lfailure
#else
	beqz s3, .Lfailure
#endif
	beqz s4, .Lfailure
	li t0, 56
	bne s1, t0, .Lfailure
	li t0, 3
	bne s2, t0, .Lfailure
	lw t0, 0(s0)
	li t1, 6
	bne t0, t1, .Lfailure
	lw t0, 56(s0)
	li t1, 3
	bne t0, t1, .Lfailure
	lw t0, 112(s0)
	li t1, 1
	bne t0, t1, .Lfailure
#ifndef ELF_FIXED_INTERPRETER
	lla t0, __image_base
	bne s3, t0, .Lfailure
#endif

	li a0, 1
	lla a1, interpreter_message
	li a2, INTERPRETER_MESSAGE_LENGTH
	li a7, 64
	ecall
	jr s4

.Lfailure:
	li a0, 1
	lla a1, failure_message
	li a2, FAILURE_MESSAGE_LENGTH
	li a7, 64
	ecall
	li a0, 1
	li a7, 93
	ecall
