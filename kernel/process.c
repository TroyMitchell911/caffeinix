#include <process.h>
#include <palloc.h>
#include <mem_layout.h>
#include <kernel_config.h>
#include <vm.h>
#include <scheduler.h>
#include <string.h>

static struct spinlock pid_lock;
static struct process proc[NPROC];
static int next_pid = 1;

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
        process->pagetable = (pagedir_t)palloc();
        if(!process->pagetable) {
                goto r1;
        }
        process->trapframe = (trapframe_t)palloc();
        if(!process->trapframe) {
                goto r2;
        }

        process->pid = pid_alloc();
        process->kstack = KSTACK((int)(process - proc));

        memset(&process->context, 0, sizeof(struct context));

        process->context.sp = process->kstack + PGSIZE;
        process->context.ra = (uint64)(proc_first_start);


r2:
        pfree(process->pagetable);
r1:
        spinlock_release(&process->lock);
        return process;
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
                vmmap(pgdir, va, pa, PGSIZE, PTE_R | PTE_W);
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