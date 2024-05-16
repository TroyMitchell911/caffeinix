/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-16
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
        ret = vm_map(pgdir, TRAPFRAME_INFO, (uint64)p->tinfo, PGSIZE, PTE_W | PTE_R);
        if(ret){
                /* We don't need free the address that PTE points because it is a code seg */
                vm_unmap(pgdir, TRAMPOLINE, 1, 0);
                pagedir_free(pgdir);
        }
        ret = vm_map(pgdir, TRAPFRAME(0), (uint64)p->cur_thread->trapframe, PGSIZE, PTE_W | PTE_R);
        if(ret){
                /* We don't need free the address that PTE points because it is a code seg */
                vm_unmap(pgdir, TRAMPOLINE, 1, 0);
                vm_unmap(pgdir, TRAPFRAME_INFO, 1, 1);
                pagedir_free(pgdir);
        }
        return pgdir;
}

void process_freepagedir(pagedir_t pgdir, uint64 sz)
{
        trapframe_info_t tinfo;
        tinfo = cur_proc()->tinfo;
        /* Save a trapframe page for exec to reuse */
        for(;tinfo->nums > 1; tinfo->nums --) {
                vm_unmap(pgdir, TRAPFRAME(tinfo->nums - 1), 1, 1);
        }
        vm_unmap(pgdir, TRAPFRAME(0), 1, 0);
        vm_unmap(pgdir, TRAPFRAME_INFO, 1, 0);
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
        spinlock_release(&cur_proc()->cur_thread->lock);
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
        spinlock_acquire(&p->cur_thread->lock);
        spinlock_release(lk);

        p->sleep_chan = chan;
        p->state = SLEEPING;
        p->cur_thread->state = READY;

        sched();

        p->sleep_chan = 0;

        spinlock_release(&p->lock);
        spinlock_release(&p->cur_thread->lock);
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
        process_t p;
        thread_t t;
        for(p = proc; p != &proc[NPROC - 1]; p++) {
                spinlock_acquire(&p->lock);
                if(p->state == UNUSED) {
                        goto found;
                }
                spinlock_release(&p->lock);
        }
found:
        t = thread_alloc(p);
        if(!t)
                goto r0;

        p->tinfo = (trapframe_info_t)palloc();
        if(!p->tinfo) {
                goto r1;
        }

        p->tinfo->nums = 1;

        p->thread[0] = p->cur_thread = t;

        /* Alloc memory for page-table */
        p->pagetable = process_pagedir(p);
        if(!p->pagetable) {
                goto r2;
        }
        
        /* Alloc pid */
        p->pid = pid_alloc();
        
        /* Set the context of return address */
        t->context.ra = (uint64)(proc_first_start);

        return p;
r2:
        p->thread[0] = p->cur_thread = 0;
        pfree(p->tinfo);
r1:
        thread_free(t);
r0:
        spinlock_release(&t->lock);
        spinlock_release(&p->lock);
        return 0;
}

static void process_free(process_t p)
{
        int i;

        if(p->pagetable) {
                process_freepagedir(p->pagetable, p->sz);
        }
        for(i = 0; i < PROC_MAXTHREAD; i++) {
                if(p->thread[i] != 0) {
                        thread_free(p->thread[i]);
                        p->thread[i] = 0;
                }
        }
        p->pid = 0;
        p->sz = 0;
        p->state = UNUSED;
        p->parent = 0;
        p->sleep_chan = 0;
        p->name[0] = 0;
}

void process_init(void)
{
        process_t p = proc;
        int i;
        /* Init the spinlock */
        spinlock_init(&pid_lock, "pid_lock");
        spinlock_init(&wait_lock, "wait_lock");
        /* Set the state and starting kernel stack address of each process */
        for(; p <= &proc[NCPU - 1]; p++) {
                spinlock_init(&p->lock, "proc");
                p->state = UNUSED;
                
                for(i = 0; i < PROC_MAXTHREAD; i++) {
                        p->thread[i] = 0;
                }
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
        thread_t t;
        char* mem;

        /* Alloc a process */
        p = process_alloc();
        t = p->cur_thread;

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
        t->trapframe->epc = 0;
        /* Set the stack pointer to highest address in memory we just alloced */
        /* IMPORTANT: NOT HIGHEST VIRTUAL ADDRESS */
        // t->trapframe->sp = PGSIZE;
        t->state = READY,

        /* Record how many memory we used */
        p->sz = PGSIZE;

        safe_strncpy(p->name, "initcode", MAXNAME);
        /* Allow schedule */
        p->state = RUNNABLE;

        /* The lock will be held in process_alloc */
        spinlock_release(&p->lock);
        spinlock_release(&t->lock);
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
        thread_t oldt, newt;

        oldp = cur_proc();
        newp = process_alloc();

        oldt = oldp->cur_thread;
        newt = newp->cur_thread;

        if(!newp)
                return -1;
        if(vm_copy(oldp->pagetable, newp->pagetable, oldp->sz) != 0) {
                process_free(newp);
                spinlock_release(&newp->lock);
                return -1;
        }

        newp->sz = oldp->sz;
        *newt->trapframe = *oldt->trapframe;

        pid = newp->pid;
        newt->trapframe->a0 = 0;

        for(i = 0; i < NOFILE; i++) {
                if(oldp->ofile[i] == 0)
                        break;
                newp->ofile[i] = file_dup(oldp->ofile[i]);
        }
        newp->cwd = idup(oldp->cwd);

        safe_strncpy(newp->name, "test", MAXNAME);

        spinlock_release(&newp->lock);
        spinlock_release(&newp->cur_thread->lock);

        spinlock_acquire(&wait_lock);
        newp->parent = oldp;
        spinlock_release(&wait_lock);

        spinlock_acquire(&newp->lock);
        newp->state = RUNNABLE;
        spinlock_acquire(&newp->cur_thread->lock);
        newp->cur_thread->state = READY;
        spinlock_release(&newp->cur_thread->lock);
        spinlock_release(&newp->lock);

        /* Return for parent process */
        return pid;
}

int process_grow(int n)
{
        uint64 sz;
        process_t p;
        
        p = cur_proc();
        sz = p->sz;
        
        if(n > 0) {
                sz = vm_alloc(p->pagetable, sz, sz + n, PTE_W); 
                if(sz == 0) {
                        return -1;
                }
        } else if(n < 0) {
                sz = vm_dealloc(p->pagetable, sz, sz + n);
        }
        p->sz = sz;
        return 0;
}