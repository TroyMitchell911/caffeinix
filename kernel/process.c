#include <process.h>
#include <palloc.h>
#include <mem_layout.h>
#include <kernel_config.h>
#include <vm.h>
#include <scheduler.h>
#include <string.h>

/* From trampoline.S */
extern char trampoline[];

static struct spinlock pid_lock;
static struct process proc[NPROC];
static int next_pid = 1;

static pagedir_t proc_pagedir(process_t p)
{
        int ret;
        pagedir_t pgdir = (pagedir_t)palloc();
        if(!pgdir) {
                return 0;
        }

        memset(pgdir, 0, PGSIZE);
        ret = vm_map(pgdir, TRAMPOLINE, (uint64)trampoline, PTE_U | PTE_W | PTE_R | PTE_X, 1);
        if(ret) {
                /* Something */
        }
        ret = vm_map(pgdir, TRAPFRAME, (uint64)p->trapframe, PTE_U | PTE_W | PTE_R | PTE_X, 1);
        if(ret){
                /* Something */
        }
        return pgdir;
}

static void proc_first_start(void)
{
        /* The function scheduler will acquire the lock */
        spinlock_release(&cur_proc()->lock);
}

static int pid_alloc(void)
{
        int pid;
        spinlock_acquire(&pid_lock);
        pid = next_pid ++;
        spinlock_release(&pid_lock);
        return pid;
}

process_t process_alloc(void)
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
        process->trapframe = (trapframe_t)palloc();
        if(!process->trapframe) {
                goto r1;
        }

        process->pagetable = proc_pagedir(process);
        if(!process->pagetable) {
                goto r2;
        }
        

        process->pid = pid_alloc();
        process->kstack = KSTACK((int)(process - proc));

        memset(&process->context, 0, sizeof(struct context));

        process->context.sp = process->kstack + PGSIZE;
        process->context.ra = (uint64)(proc_first_start);

        return process;
r2:
        pfree(process->trapframe);
r1:
        spinlock_release(&process->lock);
        return 0;
}

void process_map_kernel_stack(pagedir_t pgdir)
{
        process_t p = proc;
        uint64 pa;
        uint64 va;
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
        spinlock_init(&pid_lock, "pid_lock");
        for(; p <= &proc[NCPU - 1]; p++) {
                spinlock_init(&p->lock, "proc");
                p->state = UNUSED;
                p->kstack = KSTACK((int)(p - proc));
        }
}