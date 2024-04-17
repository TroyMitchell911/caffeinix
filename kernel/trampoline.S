.section trampsec
.global trampoline
trampoline:
.align 4
.global user_vec
user_vec:
        j user_vec

user_ret:
        j user_ret