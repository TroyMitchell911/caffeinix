/*
 * @Author: TroyMitchell
 * @Date: 2024-05-11
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-17
 * @FilePath: /caffeinix/kernel/thread.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <thread.h>
#include <palloc.h>
#include <mystring.h>
#include <uart.h>
#include <printf.h>
#include <vm.h>
#include <process.h>

struct list all_thread;

struct thread thread[NTHREAD];

static int next_tid = 1;
static struct spinlock tid_lock;

static int tid_alloc(void)
{
        int tid;
        spinlock_acquire(&tid_lock);
        tid = next_tid++;
        spinlock_release(&tid_lock);
        return tid;
}

/* Be called by vm_create */
void map_kernel_stack(pagedir_t pgdir)
{
        int i;
        uint64 pa, va;
        /* Assign kernel stack space to each process and map it */
        for(i = 0; i < NTHREAD; i++) {
                pa = (uint64)palloc();
                if(!pa) {
                        PANIC("process_map_kernel_stack");
                }
                va = KSTACK((int)(i));
                vm_map(pgdir, va, pa, PGSIZE, PTE_R | PTE_W);
        }
}

void thread_setup(void)
{
        thread_t t;
        spinlock_init(&tid_lock, "tid_lock");
        for(t = thread; t <= &thread[NTHREAD - 1]; t++) {
                spinlock_init(&t->lock, "thread");
                t->kstack = KSTACK((int)(t - thread));;
                t->state = NUSED;
                strncpy(t->name, "thread", 7);
        }
}

thread_t thread_alloc(process_t p)
{
        thread_t t;

        for(t = thread; t <= &thread[NTHREAD - 1]; t++) {
                spinlock_acquire(&t->lock);
                if(t->state == NUSED) {
                        t->home = p;
                        goto found;
                }
                spinlock_release(&t->lock);
        }
        return 0;
found:
        if(p->tnums == PROC_MAXTHREAD)
                goto r1;
        t->id_p = p->tnums ++;
        p->thread[t->id_p] = t;

        t->state = NREADY;

        t->trapframe = (trapframe_t)palloc();
        if(!t->trapframe) {
                goto r2;
        }

        memset(t->trapframe, 0, PGSIZE);

        t->tid = tid_alloc();

        /* Set the address of kernel stack */
        memset(&t->context, 0, sizeof(struct context));
        /* Set the context of stack pointer */
        t->context.sp = t->kstack + PGSIZE;

        return t;
r2:
        p->tnums --;
r1:
        spinlock_release(&t->lock);
        return 0;
}

void thread_free(thread_t t)
{
        process_t p;
        int i;

        p = t->home;

        if(p->tnums == 0)
                PANIC("thread_free");

        for(i = 0; i < PROC_MAXTHREAD; i++) {
                if(p->thread[i] == t) {
                        p->thread[i] = 0;
                        break;
                }
        }
        p->tnums --;

        t->state = NUSED;
        if(t->trapframe)
                pfree(t->trapframe);
        t->trapframe = 0;
}
