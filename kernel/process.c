/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-15
 * @FilePath: /caffeinix/kernel/process.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <process.h>
#include <palloc.h>
#include <mem_layout.h>
#include <kernel_config.h>
#include <vm.h>
#include <scheduler.h>
#include <mystring.h>
#include <file.h>
#include <dirent.h>

/* From trampoline.S */
extern char trampoline[];

static struct spinlock pid_lock, wait_lock;
struct process proc[NPROC];
static int next_pid = 1;

pagedir_t process_pagedir(process_t p)
{
        int ret;
        pagedir_t pgdir;
        /* Malloc memory for page-talble */
        pgdir = pagedir_alloc();
        /* Map trampoline */
        ret = vm_map(pgdir, TRAMPOLINE, (uint64)trampoline, PGSIZE, PTE_R | PTE_X);
        if(ret) {
                pagedir_free(pgdir);
                return 0;
        }
        /* Map address of under trampoline to trapframe */
        ret = vm_map(pgdir, TRAPFRAME, (uint64)p->trapframe, PGSIZE, PTE_W | PTE_R);
        if(ret){
                /* We don't need free the address that PTE points because it is a code seg */
                vm_unmap(pgdir, TRAMPOLINE, 1, 0);
                pagedir_free(pgdir);
        }
        return pgdir;
}

void process_freepagedir(pagedir_t pgdir, uint64 sz)
{
        vm_unmap(pgdir, TRAPFRAME, 1, 0);
        vm_unmap(pgdir, TRAMPOLINE, 1, 0);
        vm_unmap(pgdir, 0, PGROUNDUP(sz) / PGSIZE, 1);
        pagedir_free(pgdir);
}

/* This function is the first when a process first start */
static void proc_first_start(void)
{
        static uint8 first = 0;
        /* The function scheduler will acquire the lock */
        spinlock_release(&cur_proc()->lock);
        if(!first) {
                first = 1;
                fs_init(1);
        }
        extern void user_trap_ret(void);
        user_trap_ret();
}

/* All processes have a alone id, pid */
static int pid_alloc(void)
{
        int pid;
        spinlock_acquire(&pid_lock);
        pid = next_pid ++;
        spinlock_release(&pid_lock);
        return pid;
}
#ifndef PROCESS_NO_SCHED
void sleep(void* chan, spinlock_t lk)
{
        process_t p = cur_proc();

        spinlock_acquire(&p->lock);
        spinlock_release(lk);

        p->sleep_chan = chan;
        p->state = SLEEPING;

        sched();

        p->sleep_chan = 0;

        spinlock_release(&p->lock);
        spinlock_acquire(lk);
}

void wakeup(void* chan)
{
        process_t p;
        for(p = proc; p != &proc[NPROC - 1]; p++) {
                if(p != cur_proc()) {
                        spinlock_acquire(&p->lock);
                        if(p->sleep_chan == chan && p->state == SLEEPING) {
                                p->state = RUNNABLE;
                        }
                        spinlock_release(&p->lock);
                }
        }
}
void sleep_(void* chan, spinlock_t lk)
{
        sleep(chan, lk);
}
void wakeup_(void* chan)
{
        wakeup(chan);
}
#else
/* TODO */
static volatile uint8 test_flag = 0;
void sleep_(void* chan, spinlock_t lk)
{
        test_flag = 1;
        spinlock_release(lk);
        intr_on();
        while(test_flag);
        spinlock_acquire(lk);
}

void wakeup_(void* chan)
{
        test_flag = 0;
}
void sleep(void* chan, spinlock_t lk)
{
 
}

void wakeup(void* chan)
{

}
#endif
/* Alloc a process */
static process_t process_alloc(void)
{
        process_t process;
        for(process = proc; process != &proc[NPROC - 1]; process++) {
                spinlock_acquire(&process->lock);
                if(process->state == UNUSED) {
                        goto found;
                }
                spinlock_release(&process->lock);
        }
found:
        /* Alloc memory for trapframe */
        process->trapframe = (trapframe_t)palloc();
        if(!process->trapframe) {
                goto r1;
        }
        /* Alloc memory for page-table */
        process->pagetable = process_pagedir(process);
        if(!process->pagetable) {
                goto r2;
        }
        
        /* Alloc pid */
        process->pid = pid_alloc();
        /* Set the address of kernel stack */
        process->kstack = KSTACK((int)(process - proc));
        /* Clear the context of process */
        memset(&process->context, 0, sizeof(struct context));
        /* Set the context of stack pointer */
        process->context.sp = process->kstack + PGSIZE;
        /* Set the context of return address */
        process->context.ra = (uint64)(proc_first_start);

        return process;
r2:
        pfree(process->trapframe);
r1:
        spinlock_release(&process->lock);
        return 0;
}

static void process_free(process_t p)
{
        if(p->pagetable) {
                process_freepagedir(p->pagetable, p->sz);
        }
        if(p->trapframe)
                pfree(p->trapframe);
        p->trapframe = 0;
        p->pid = 0;
        p->sz = 0;
        p->state = UNUSED;
        p->parent = 0;
        p->sleep_chan = 0;
        p->name[0] = 0;
}

/* Be called by vm_create */
void process_map_kernel_stack(pagedir_t pgdir)
{
        process_t p = proc;
        uint64 pa;
        uint64 va;
        /* Assign kernel stack space to each process and map it */
        for(; p <= &proc[NCPU - 1]; p++) {
                pa = (uint64)palloc();
                if(!pa) {
                        PANIC("process_map_kernel_stack");
                }
                va = KSTACK((int)(p - proc));
                vm_map(pgdir, va, pa, PGSIZE, PTE_R | PTE_W);
        }
}

void process_init(void)
{
        process_t p = proc;
        /* Init the spinlock */
        spinlock_init(&pid_lock, "pid_lock");
        spinlock_init(&wait_lock, "wait_lock");
        /* Set the state and starting kernel stack address of each process */
        for(; p <= &proc[NCPU - 1]; p++) {
                spinlock_init(&p->lock, "proc");
                p->state = UNUSED;
                p->kstack = KSTACK((int)(p - proc));
        }
}

/* od -t xC ./user/initcode */
static uint8 initcode[] = {
        0x17, 0x05, 0x00, 0x00, 0x13, 0x05, 0x45, 0x02,
        0x97, 0x05, 0x00, 0x00, 0x93, 0x85, 0x35, 0x02,
        0x93, 0x08, 0x70, 0x00, 0x73, 0x00, 0x00, 0x00,
        0x93, 0x08, 0x20, 0x00, 0x73, 0x00, 0x00, 0x00,
        0xef, 0xf0, 0x9f, 0xff, 0x2f, 0x69, 0x6e, 0x69,
        0x74, 0x00, 0x00, 0x24, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00
};

void userinit(void)
{
        process_t p;
        char* mem;
        /* Alloc a process */
        p = process_alloc();

        if(!p) {
                PANIC("userinit");
        }

        if(sizeof(initcode) > PGSIZE) {
                PANIC("userinit");
        }
        /* Alloc physical memory for the process that we just alloced */
        mem = palloc();
        if(!mem) {
                PANIC("userinit palloc");
        }
        memset(mem, 0, PGSIZE);
        /* Map the lowest virtual address */
        vm_map(p->pagetable, 0, (uint64)mem, PGSIZE, PTE_U | PTE_R | PTE_W | PTE_X);
        /* Copy the code of first process into the memory that we just alloced */
        memmove(mem, initcode, sizeof(initcode));

        p->cwd = namei("/");

        /* Set the epc to '0' because we have mapped the code to lowest address */
        p->trapframe->epc = 0;
        /* Set the stack pointer to highest address in memory we just alloced */
        /* IMPORTANT: NOT HIGHEST VIRTUAL ADDRESS */
        p->trapframe->sp = PGSIZE;
        /* Record how many memory we used */
        p->sz = PGSIZE;

        safe_strncpy(p->name, "initcode", MAXNAME);
        /* Allow schedule */
        p->state = RUNNABLE;

        /* The lock will be held in process_alloc */
        spinlock_release(&p->lock);
}

int either_copyout(int user_dst, uint64 dst, void* src, uint64 len)
{
        process_t p = cur_proc();

        if(user_dst) {
                return copyout(p->pagetable, dst, (char*)src, len);
        } else {
                memmove((char*)dst, src, len);
                return 0;
        }
}

int either_copyin(void *dst, int user_src, uint64 src, uint64 len)
{
        process_t p = cur_proc();

        if(user_src) {
                return copyin(p->pagetable, (char*)dst, src, len);
        } else {
                memmove(dst, (char*)src, len);
                return 0;
        }
}

/* TODO: test */
int fork(void)
{
        int pid, i;
        process_t oldp, newp;

        oldp = cur_proc();
        newp = process_alloc();

        if(!newp)
                return -1;
        if(vm_copy(oldp->pagetable, newp->pagetable, oldp->sz) != 0) {
                process_free(newp);
                spinlock_release(&newp->lock);
                return -1;
        }

        newp->sz = oldp->sz;
        *newp->trapframe = *oldp->trapframe;

        pid = newp->pid;
        newp->trapframe->a0 = 0;

        for(i = 0; i < NOFILE; i++) {
                if(oldp->ofile[i] == 0)
                        break;
                newp->ofile[i] = file_dup(oldp->ofile[i]);
        }
        newp->cwd = idup(oldp->cwd);

        safe_strncpy(newp->name, "test", MAXNAME);

        spinlock_release(&newp->lock);

        spinlock_acquire(&wait_lock);
        newp->parent = oldp;
        spinlock_release(&wait_lock);

        spinlock_acquire(&newp->lock);
        newp->state = RUNNABLE;
        spinlock_release(&newp->lock);

        /* Return for parent process */
        return pid;
}