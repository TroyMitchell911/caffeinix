/*
 * @Author: TroyMitchell
 * @Date: 2024-05-07
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-14
 * @FilePath: /caffeinix/kernel/syscall.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include "typedefs.h"
#include <syscall.h>
#include <scheduler.h>
#include <debug.h>
#include <mystring.h>
#include <vm.h>
#include <printf.h>

static uint64 argraw(int n)
{
        process_t p = cur_proc(); 
        uint64 *args = &p->trapframe->a0;
        if(n <= 5)
                return args[n];

        PANIC("argraw");
        return -1;
}

int fetch_str_from_user(uint64 user_addr, char* buf, int max)
{
        process_t p = cur_proc();
        if(copyinstr(p->pagetable, buf, user_addr, max) < 0) {
                return -1;
        }
        return strlen(buf);
}

int fetch_addr_from_user(uint64 user_addr, uint64* dst)
{
        process_t p = cur_proc();
        if(user_addr >= p->sz || user_addr + sizeof(uint64) > p->sz) 
                return -1;
        if(copyin(p->pagetable, (char*)dst, user_addr, sizeof(uint64)) != 0) {
                return -1;
        }
        return 0;
}

void argint(int n, int *ip)
{
        *ip = argraw(n);
}

void argaddr(int n, uint64 *ap)
{
        *ap = argraw(n);
}

int argstr(int n, char *buf, int max)
{
        uint64 addr;
        /* Get the address of string */
        argaddr(n, &addr);
        return fetch_str_from_user(addr, buf, max);
}

extern uint64 sys_open(void);
extern uint64 sys_close(void);
extern uint64 sys_read(void);
extern uint64 sys_exec(void);
extern uint64 sys_mknod(void);
extern uint64 sys_write(void);
extern uint64 sys_dup(void);
extern uint64 sys_getpid(void);
extern uint64 sys_fork(void);
extern uint64 sys_mkdir(void);

typedef uint64 (*syscall_t)(void);

syscall_t syscalls[] = {
        [SYS_close] = sys_close,
        [SYS_open] = sys_open,
        [SYS_read] = sys_read,
        [SYS_exec] = sys_exec,
        [SYS_mknod] = sys_mknod,
        [SYS_write] = sys_write,
        [SYS_dup] = sys_dup,
        [SYS_getpid] = sys_getpid,
        [SYS_fork] = sys_fork,
        [SYS_mkdir] = sys_mkdir,
};      

void syscall(void)
{
        int syscall_num;
        process_t p = cur_proc();

        syscall_num = p->trapframe->a7;
        if(syscall_num > 0 && syscall_num < NELEM(syscalls) && syscalls[syscall_num]) {
                // printf("syscall_num: %d\n", syscall_num);
                p->trapframe->a0 = syscalls[syscall_num]();
        } else {
                p->trapframe->a0 = -1;
                printf("Unknown syscall number %d from this process-> pid:%d name:%s\n", 
                        syscall_num, p->pid, p->name);
                PANIC("unknown");
        }
}