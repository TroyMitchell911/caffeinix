.section .text
.global _entry

_entry:
    # Load the stack address into sp register
    la sp, stack_for_c
    # Load hart id into x0 register
    csrr x1, mhartid
    # Add the value of hartid because we wanna make the stack pointers top instead of bottom
    addi x1, x1, 1
    # Multiply the value of hartid by 4096 
    slli x1, x1, 12
    # The sp pointer of any hart points to the top of the stack
    add sp, sp, x1
    # Go into C environment
    j setup
spin:
    j spin