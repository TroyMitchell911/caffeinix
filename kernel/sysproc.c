/*
 * @Author: TroyMitchell
 * @Date: 2024-05-31
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-31
 * @FilePath: /caffeinix/kernel/sysproc.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */

#include <spinlock.h>
#include <typedefs.h>
#include <process.h>
#include <vm.h>
#include <mystring.h>
#include <syscall.h>
#include <scheduler.h>
#include <mem_layout.h>

static void clone_first_start(void)
{
        /* The function scheduler will acquire the lock */
        spinlock_release(&cur_proc()->lock);
        spinlock_release(&cur_proc()->cur_thread->lock);
        extern void user_trap_ret(void);
        user_trap_ret();
}

uint64 sys_clone(void)
{
        uint64 func_addr, arg_addr, sz;
        process_t p;
        thread_t t;
        int ret;

        argaddr(0, &func_addr);
        argaddr(3, &arg_addr);

        p = cur_proc();
        t = thread_alloc(p);
        if(!t)
                goto r0;
        
        sz = p->sz;
        sz = vm_alloc(p->pagetable, sz, sz + PGSIZE * 2, PTE_W);
        if(sz == 0)
                goto r1;

        ret = vm_map(p->pagetable, TRAPFRAME(p->tnums - 1), (uint64)t->trapframe, PGSIZE, PTE_W | PTE_R);
        if(ret)
                goto r2;

        p->sz = sz;

        t->context.ra = (uint64)clone_first_start;
        t->trapframe->epc = func_addr;
        t->trapframe->a0 = arg_addr;
        t->trapframe->sp = sz;
        t->state = READY;
        strncpy(t->name, "clone", 6);

        spinlock_release(&t->lock);

        return 0;
r2:
        vm_dealloc(p->pagetable, sz, p->sz);
r1:     
        thread_free(t);
r0:
        return -1;
}

extern volatile uint64 tick_count;
extern struct spinlock tick_lock;
uint64 sys_sleep(void)
{
        int n;

        argint(0, &n);

        spinlock_acquire(&tick_lock);

        n = n * 1000 / TICK_INTERVAL + tick_count;
        while(n > tick_count) {
                /* TODO: Is killed? */
                sleep((void*)&tick_count, &tick_lock);
        }
        spinlock_release(&tick_lock);

        return 0;
}

extern void exit(int cause);
uint64 sys_exit(void)
{
        int n;
        argint(0, &n);
        exit(n);
        /* Never reach here */
        return 0;
}

extern int kill(int pid);
uint64 sys_kill(void)
{
        int pid;
        
        argint(0, &pid);

        return kill(pid);
}

extern int wait(uint64 addr);
uint64 sys_wait(void)
{
        uint64 addr;

        argaddr(0, &addr);

        return wait(addr);
}

extern int fork(void);
uint64 sys_fork(void)
{
        return fork();
}

uint64 sys_getcwd(void)
{
        process_t p;
        uint64 addr;
        int n, ret;
        char path[MAXPATH];

        argaddr(0, &addr);
        argint(1, &n);

        n = n > MAXPATH ? MAXPATH : n;

        p = cur_proc();
        strncpy(path, p->cwd_name, n);
        /* Get the length of path string: the real length or original n */
        n = strlen(path) + 1;
        ret = copyout(p->pagetable, addr, path, n);
        if(ret)
                return -1;
        
        return 0;
}