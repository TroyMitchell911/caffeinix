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
struct process proc[NPROC];
static int next_pid = 1;

static pagedir_t proc_pagedir(process_t p)
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

/* This function is the first when a process first start */
static void proc_first_start(void)
{
        /* The function scheduler will acquire the lock */
        spinlock_release(&cur_proc()->lock);
        
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

/* Alloc a process */
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
        /* Alloc memory for trapframe */
        process->trapframe = (trapframe_t)palloc();
        if(!process->trapframe) {
                goto r1;
        }
        /* Alloc memory for page-table */
        process->pagetable = proc_pagedir(process);
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

        /* Set the epc to '0' because we have mapped the code to lowest address */
        p->trapframe->epc = 0;
        /* Set the stack pointer to highest address in memory we just alloced */
        /* IMPORTANT: NOT HIGHEST VIRTUAL ADDRESS */
        p->trapframe->sp = PGSIZE;
        /* Record how many memory we used */
        p->sz = PGSIZE;

        p->name = "initcode";
        /* Allow schedule */
        p->state = RUNNABLE;

        /* The lock will be held in process_alloc */
        spinlock_release(&p->lock);
}