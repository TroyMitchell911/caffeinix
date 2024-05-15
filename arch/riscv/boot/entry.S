.section .text
.global _entry

_entry:
    # Load the stack address into sp register 加载堆栈地址到sp寄存器
    la sp, stack_for_c
    # Load hart id into x0 register 将hart ID 加载到x0寄存器
    csrr x1, mhartid
    # Add the value of hartid because we wanna make the stack pointers top instead of bottomv 加上hartid的值因为我们想把堆栈指针放在顶部而不是底部
    addi x1, x1, 1
    # Multiply the value of hartid by 4096  hartid的值乘以4096
    slli x1, x1, 12
    # The sp pointer of any hart points to the top of the stack  任何hart的sp指针都指向堆栈的顶部
    add sp, sp, x1
    # Go into C environment
    j setup
spin:
    j spin