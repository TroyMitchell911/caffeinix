.section .text
.global test_clone
.type test_clone, %function
test_clone:
	andi a1, a1, -16
	addi a1, a1, -16
	sd a0, 0(a1)
	sd a3, 8(a1)
	mv a0, a2
	mv a2, a4
	mv a3, a5
	mv a4, a6
	li a7, 220
	ecall
	beqz a0, .Lchild
	ret

.Lchild:
	ld a1, 0(sp)
	ld a0, 8(sp)
	jalr a1
	li a7, 93
	ecall
	unimp
.size test_clone, .-test_clone
