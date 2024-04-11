.text
.global _entry

_entry:
    li x5, 3
    li x6, 4
    add x5, x5, x6
    j _entry