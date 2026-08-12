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
#include <mem_layout.h>
#include <palloc.h>
#include <mystring.h>
#include <printf.h>
#include <vm.h>
#include <process.h>

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
	int i, page;
        uint64 pa, va;
        /* Assign kernel stack space to each process and map it */
        for(i = 0; i < NTHREAD; i++) {
		va = KSTACK(i);
		for (page = 0; page < KSTACK_PAGES; page++) {
			pa = (uint64)palloc();
			if (!pa)
				PANIC("process_map_kernel_stack");
			vm_map(pgdir, va + page * PGSIZE, pa, PGSIZE,
			       PTE_R | PTE_W);
		}
        }
}

void thread_setup(void)
{
        thread_t t;
        spinlock_init(&tid_lock, "tid_lock");
        for(t = thread; t <= &thread[NTHREAD - 1]; t++) {
                spinlock_init(&t->lock, "thread");
                t->kstack = KSTACK((int)(t - thread));;
                t->state = THREAD_UNUSED;
                t->on_runqueue = 0;
                list_init(&t->run_node);
                t->waiting_on = 0;
                t->on_waitqueue = 0;
                list_init(&t->wait_node);
                strncpy(t->name, "thread", 7);
        }
}

thread_t thread_alloc(process_t p)
{
        thread_t t;

        for(t = thread; t <= &thread[NTHREAD - 1]; t++) {
                int ret = spinlock_trylock(&t->lock);
		if (ret)
			continue;

                if(t->state == THREAD_UNUSED) {
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

        t->state = THREAD_ALLOCATED;
        t->on_runqueue = 0;
        list_init(&t->run_node);
        t->waiting_on = 0;
        t->on_waitqueue = 0;
        list_init(&t->wait_node);

        t->trapframe = (trapframe_t)palloc();
        if(!t->trapframe) {
                goto r2;
        }

        memset(t->trapframe, 0, PGSIZE);

        t->tid = tid_alloc();

        /* Set the address of kernel stack */
        memset(&t->context, 0, sizeof(struct context));
        /* Set the context of stack pointer */
		t->context.sp = t->kstack + KSTACK_SIZE;

        return t;
r2:
        p->tnums --;
        p->thread[t->id_p] = 0;
r1:
        t->state = THREAD_UNUSED;
        t->home = 0;
        spinlock_release(&t->lock);
        return 0;
}

void thread_free(thread_t t)
{
        process_t p;
        int i;

        p = t->home;

        if(t->on_runqueue)
                PANIC("thread_free runnable");
        if(t->on_waitqueue || t->waiting_on)
                PANIC("thread_free waiting");
        if(p->tnums == 0)
                PANIC("thread_free");

        for(i = 0; i < PROC_MAXTHREAD; i++) {
                if(p->thread[i] == t) {
                        p->thread[i] = 0;
                        break;
                }
        }
        p->tnums --;

        t->state = THREAD_UNUSED;
        if(t->trapframe)
                pfree(t->trapframe);
        t->trapframe = 0;
        t->home = 0;
        list_init(&t->run_node);
        list_init(&t->wait_node);
}
