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
#include <scheduler.h>

struct thread thread[NTHREAD];

static int next_tid = 1;
static int next_kernel_tid = -1;
static struct spinlock tid_lock;

static void thread_sched_init(thread_t thread)
{
	rb_node_init(&thread->sched.run_node);
	thread->sched.vruntime = 0;
	thread->sched.exec_start = 0;
	thread->sched.sum_exec_runtime = 0;
	thread->sched.slice_ns = 0;
	thread->sched.weight = 1024;
	thread->sched.nice = 0;
	thread->sched.initialized = 0;
	thread->sched.on_runqueue = 0;
}

static void kernel_thread_entry(void)
{
	thread_t current = cur_thread();
	thread_func_t function = current->kernel_function;
	void *argument = current->kernel_argument;

	spinlock_release(&current->lock);
	intr_on();
	function(argument);
	scheduler_exit();
}

static int tid_alloc(void)
{
        int tid;
        spinlock_acquire(&tid_lock);
        tid = next_tid++;
        spinlock_release(&tid_lock);
        return tid;
}

static int kernel_tid_alloc(void)
{
	int tid;

	spinlock_acquire(&tid_lock);
	tid = next_kernel_tid--;
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
		thread_sched_init(t);
                t->waiting_on = 0;
                t->on_waitqueue = 0;
                list_init(&t->wait_node);
		t->on_timeout_queue = 0;
		list_init(&t->timeout_node);
                strncpy(t->name, "thread", 7);
        }
}

thread_t thread_alloc(process_t p)
{
        thread_t t;
	int slot;

	if (!p || !spinlock_holding(&p->lock))
		PANIC("thread_alloc process lock");
	if (p->tnums == PROC_MAXTHREAD)
		return 0;
	for (slot = 0; slot < PROC_MAXTHREAD; slot++) {
		if (!p->thread[slot])
			break;
	}
	if (slot == PROC_MAXTHREAD)
		PANIC("thread slot count");

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
	t->id_p = slot;
        p->thread[t->id_p] = t;
	p->tnums++;
	p->live_threads++;

        t->state = THREAD_ALLOCATED;
	t->lwip_errno = 0;
	t->clear_child_tid = 0;
	t->signal_mask = 0;
	t->process_reaper = 0;
	t->exit_requested = 0;
	t->exit_status = 0;
	thread_sched_init(t);
        t->waiting_on = 0;
        t->on_waitqueue = 0;
        list_init(&t->wait_node);
	t->on_timeout_queue = 0;
	list_init(&t->timeout_node);
	t->kernel_function = 0;
	t->kernel_argument = 0;
	t->kernel_thread = 0;

	t->trapframe = (trapframe_t)palloc_zero();
        if(!t->trapframe) {
                goto r2;
        }

        t->tid = tid_alloc();

        /* Set the address of kernel stack */
        memset(&t->context, 0, sizeof(struct context));
        /* Set the context of stack pointer */
		t->context.sp = t->kstack + KSTACK_SIZE;

        return t;
r2:
	p->live_threads--;
	p->tnums--;
        p->thread[t->id_p] = 0;
        t->state = THREAD_UNUSED;
        t->home = 0;
        spinlock_release(&t->lock);
        return 0;
}

thread_t kernel_thread_create(const char *name, thread_func_t function,
			      void *argument)
{
	thread_t t;

	if (!name || !function)
		return 0;
	for (t = thread; t <= &thread[NTHREAD - 1]; t++) {
		if (spinlock_trylock(&t->lock))
			continue;
		if (t->state == THREAD_UNUSED)
			goto found;
		spinlock_release(&t->lock);
	}
	return 0;

found:
	t->state = THREAD_ALLOCATED;
	t->tid = kernel_tid_alloc();
	t->id_p = -1;
	t->home = 0;
	t->trapframe = 0;
	t->kernel_function = function;
	t->kernel_argument = argument;
	t->kernel_thread = 1;
	t->lwip_errno = 0;
	t->clear_child_tid = 0;
	t->signal_mask = 0;
	t->process_reaper = 0;
	t->exit_requested = 0;
	t->exit_status = 0;
	thread_sched_init(t);
	t->waiting_on = 0;
	t->on_waitqueue = 0;
	list_init(&t->wait_node);
	t->on_timeout_queue = 0;
	list_init(&t->timeout_node);
	memset(&t->context, 0, sizeof(t->context));
	t->context.ra = (uint64)kernel_thread_entry;
	t->context.sp = t->kstack + KSTACK_SIZE;
	safe_strncpy(t->name, name, sizeof(t->name));
	scheduler_make_runnable(t);
	spinlock_release(&t->lock);
	return t;
}

void kernel_thread_reap(thread_t t)
{
	if (!t || !spinlock_holding(&t->lock) || !t->kernel_thread ||
	    t->state != THREAD_EXITED || t->sched.on_runqueue ||
	    t->on_waitqueue || t->on_timeout_queue)
		PANIC("reap kernel thread");
	t->state = THREAD_UNUSED;
	t->tid = 0;
	t->kernel_thread = 0;
	t->kernel_function = 0;
	t->kernel_argument = 0;
	t->lwip_errno = 0;
	t->clear_child_tid = 0;
	t->signal_mask = 0;
	t->process_reaper = 0;
	t->exit_requested = 0;
	t->exit_status = 0;
	thread_sched_init(t);
	list_init(&t->wait_node);
	list_init(&t->timeout_node);
	safe_strncpy(t->name, "thread", sizeof(t->name));
}

void thread_free(thread_t t)
{
        process_t p;
        int i;

	p = t->home;
	if (!p || !spinlock_holding(&p->lock) ||
	    !spinlock_holding(&t->lock))
		PANIC("thread_free locks");

	if (t->sched.on_runqueue)
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
	p->tnums--;
	if (t->state != THREAD_EXITED) {
		if (!p->live_threads)
			PANIC("thread_free live count");
		p->live_threads--;
	}

        t->state = THREAD_UNUSED;
        if(t->trapframe)
                pfree(t->trapframe);
	t->trapframe = 0;
	t->home = 0;
	t->tid = 0;
	t->id_p = -1;
	t->clear_child_tid = 0;
	t->signal_mask = 0;
	t->process_reaper = 0;
	t->exit_requested = 0;
	t->exit_status = 0;
	thread_sched_init(t);
        list_init(&t->wait_node);
	list_init(&t->timeout_node);
}

void user_thread_reap(thread_t t)
{
	process_t p;
	int slot;

	if (!t || !spinlock_holding(&t->lock) || t->kernel_thread ||
	    t->state != THREAD_EXITED || t->process_reaper)
		PANIC("reap user thread");
	p = t->home;
	if (!p)
		PANIC("reap detached thread");
	/* Keep the global process -> thread lock order. */
	spinlock_release(&t->lock);
	spinlock_acquire(&p->lock);
	spinlock_acquire(&t->lock);
	if (t->home != p || t->state != THREAD_EXITED ||
	    t->process_reaper)
		PANIC("changed user reaper");
	slot = t->id_p;
	if (slot < 0 || slot >= PROC_MAXTHREAD || p->thread[slot] != t)
		PANIC("reap user slot");
	if (vm_mapped(p->pagetable, TRAPFRAME(slot)))
		vm_unmap(p->pagetable, TRAPFRAME(slot), 1, 0);
	thread_free(t);
	wait_queue_wake_all(&p->thread_reap_wait);
	spinlock_release(&p->lock);
}
