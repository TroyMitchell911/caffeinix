/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-30
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
#include <block_device.h>
#include <vfs.h>

/* From trampoline.S */
extern char trampoline[];
extern int exec_linux(char *path, char **argv, char **envp);

static struct spinlock pid_lock, wait_lock;
// struct process proc[NPROC];
struct list proc;
static int next_pid = 1;
static process_t first;

static int setup_stdio(void)
{
	process_t p = cur_proc();
	int fd;

	if (p->ofile[0] || p->ofile[1] || p->ofile[2])
		return -1;
	if (vfs_open("/dev/console", VFS_OPEN_READ | VFS_OPEN_WRITE, 0,
	             &fd) < 0 ||
	    fd != 0)
		return -1;
	if (vfs_dup(0, 0, 0, &fd) < 0 || fd != 1)
		return -1;
	if (vfs_dup(0, 0, 0, &fd) < 0 || fd != 2)
		return -1;
	return 0;
}

static void reparent(process_t p)
{
        process_t pp;
        list_t l;

        for(l = proc.next; l != &proc; l = l->next) {
                pp = list_entry(l, struct process, all_tag);
                if(!pp)
                        continue;
                if(pp != p) {
                        spinlock_acquire(&pp->lock);
                        if(pp->parent == p) {
                                pp->parent = first;
                                spinlock_release(&pp->lock);
                                wakeup(pp->parent);
                                continue;
                        }  
                        spinlock_release(&pp->lock);
                }
        }
}

pagedir_t process_pagedir(process_t p)
{
        int ret;
        pagedir_t pgdir;
        /* Malloc memory for page-talble */
        pgdir = pagedir_alloc();
	if (!pgdir)
		return 0;
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
		return 0;
	}
        ret = vm_map(pgdir, TRAPFRAME(0), (uint64)p->cur_thread->trapframe, PGSIZE, PTE_W | PTE_R);
	if(ret){
                /* We don't need free the address that PTE points because it is a code seg */
		vm_unmap(pgdir, TRAMPOLINE, 1, 0);
		vm_unmap(pgdir, TRAPFRAME_INFO, 1, 0);
		pagedir_free(pgdir);
		return 0;
	}
        return pgdir;
}

void process_freepagedir(pagedir_t pgdir, uint64 sz)
{
	int i;

	(void)sz;
	for (i = 0; i < PROC_MAXTHREAD; i++) {
		if (vm_mapped(pgdir, TRAPFRAME(i)))
			vm_unmap(pgdir, TRAPFRAME(i), 1, 0);
	}
	if (vm_mapped(pgdir, TRAPFRAME_INFO))
		vm_unmap(pgdir, TRAPFRAME_INFO, 1, 0);
	if (vm_mapped(pgdir, TRAMPOLINE))
		vm_unmap(pgdir, TRAMPOLINE, 1, 0);
	vm_free_user(pgdir);
	pagedir_free(pgdir);
}

/* This function is the first when a process first start */
static void proc_first_start(void)
{
	static uint8 fs_started;
	char *argv[] = { INIT_PATH, 0 };
	char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		"TERM=vt100",
		0,
	};
	process_t process = cur_proc();

        /* The function scheduler will acquire the lock */
        spinlock_release(&cur_proc()->lock);
        spinlock_release(&cur_proc()->cur_thread->lock);
	if (!fs_started) {
		fs_started = 1;
		if (vfs_mount_root(ROOT_FILESYSTEM, block_device_get(1), 0) < 0)
			PANIC("mount root");
		if (vfs_get_root(&process->root) < 0)
			PANIC("process root");
		vfs_path_copy(&process->cwd, &process->root);
		if (setup_stdio() < 0)
			PANIC("stdio setup");
		if (exec_linux(INIT_PATH, argv, envp) < 0)
			PANIC("exec init");
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
        thread_t t = p->cur_thread;

        spinlock_acquire(&p->lock);
        spinlock_acquire(&t->lock);
        spinlock_release(lk);

        t->sleep_chan = chan;
        p->state = RUNNABLE;
        t->state = RESETING;

        sched();

        t->sleep_chan = 0;

        spinlock_release(&p->lock);
        spinlock_release(&t->lock);
        spinlock_acquire(lk);
}

void wakeup(void* chan)
{
        process_t p;
        thread_t t;
        int i;
        list_t l;

        for(l = proc.next; l != &proc; l = l->next) {
                p = list_entry(l, struct process, all_tag);
                if(!p)
                        continue;
                if(p != cur_proc()) {
                        spinlock_acquire(&p->lock);
                        if(p->tnums != 0) {
                                for(i = 0; i < p->tnums; i++) {
                                        t = p->thread[i];
                                        if(t->sleep_chan == chan && t->state == RESETING)
                                                t->state = READY;
                                }
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
        int i;
        
        p = malloc(sizeof(struct process));
        if(!p)
                return 0;
	memset(p, 0, sizeof(*p));
	p->umask = 0022;
        
        spinlock_init(&p->lock, "process");
        spinlock_acquire(&p->lock);

        for(i = 0; i < PROC_MAXTHREAD; i++) {
                p->thread[i] = 0;
        }

        p->tnums = 0;

        t = thread_alloc(p);
        if(!t)
                goto r0;

        p->tinfo = (trapframe_info_t)palloc();
        if(!p->tinfo) {
                goto r1;
        }

        p->tinfo->nums = 1;

        p->cur_thread = t;

        /* Alloc memory for page-table */
        p->pagetable = process_pagedir(p);
        if(!p->pagetable) {
                goto r2;
        }
        
        /* Alloc pid */
        p->pid = pid_alloc();
        
        /* Set the context of return address */
        t->context.ra = (uint64)(proc_first_start);

        list_init(&p->all_tag);
        list_insert_after(&proc, &p->all_tag);

        return p;
r2:
	p->cur_thread = 0;
	pfree(p->tinfo);
r1:
	thread_free(t);
	spinlock_release(&t->lock);
r0:
	spinlock_release(&p->lock);
	free(p);
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
	if (p->tinfo)
		pfree(p->tinfo);
        list_remove(&p->all_tag);
        free(p);
        // p->pid = 0;
        // p->sz = 0;
        // p->state = UNUSED;
        // p->parent = 0;
        // p->sleep_chan = 0;
        // p->name[0] = 0;
}

void process_init(void)
{
        // process_t p = proc;
        // int i;
        /* Init the spinlock */
        spinlock_init(&pid_lock, "pid_lock");
        spinlock_init(&wait_lock, "wait_lock");
        list_init(&proc);
        /* Set the state and starting kernel stack address of each process */
        // for(; p <= &proc[NCPU - 1]; p++) {
        //         spinlock_init(&p->lock, "proc");
        //         p->state = UNUSED;
        //         p->tnums = 0;
        //         for(i = 0; i < PROC_MAXTHREAD; i++) {
        //                 p->thread[i] = 0;
        //         }
        // }
}

void userinit(void)
{
        process_t p;
        thread_t t;

        /* Alloc a process */
        p = process_alloc();
	if (!p)
                PANIC("userinit");
	t = p->cur_thread;
	t->state = READY;
	p->sz = 0;
	p->brk = 0;
	p->brk_start = 0;
	p->mmap_top = USER_MMAP_TOP;

	safe_strncpy(p->name, "kernel-init", MAXNAME);
        /* Allow schedule */
        p->state = RUNNABLE;

        first = p;

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

int process_fork(uint64 child_stack)
{
        int pid, i;
        process_t oldp, newp;
        thread_t oldt, newt;

        oldp = cur_proc();
        newp = process_alloc();
	if(!newp)
		return -1;

        oldt = oldp->cur_thread;
        newt = newp->cur_thread;

	if(vm_copy(oldp->pagetable, newp->pagetable) != 0) {
		spinlock_release(&newt->lock);
		spinlock_release(&newp->lock);
		process_free(newp);
		return -1;
	}

        newp->sz = oldp->sz;
	newp->brk = oldp->brk;
	newp->brk_start = oldp->brk_start;
	newp->mmap_top = oldp->mmap_top;
	newp->umask = oldp->umask;
	*newt->trapframe = *oldt->trapframe;
	if (child_stack)
		newt->trapframe->sp = child_stack;

        pid = newp->pid;
        newt->trapframe->a0 = 0;

        for(i = 0; i < NOFILE; i++) {
		if(oldp->ofile[i])
			newp->ofile[i] = file_dup(oldp->ofile[i]);
		newp->fd_flags[i] = oldp->fd_flags[i];
        }
	vfs_path_copy(&newp->root, &oldp->root);
	vfs_path_copy(&newp->cwd, &oldp->cwd);
        /* Copy the process name into newp */
	safe_strncpy(newp->name, oldp->name, MAXNAME);
	newp->signal_mask = oldp->signal_mask;
	memmove(newp->signal_actions, oldp->signal_actions,
	        sizeof(newp->signal_actions));

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

void exit(int cause)
{
        process_t p;
        file_t f;
        int fd, i;

        p = cur_proc();

        for(fd = 0; fd < NOFILE; fd++) {
                if((f = p->ofile[fd]) != 0) {
                        file_close(f);
                        p->ofile[fd] = 0;
                }
        }

	vfs_path_put(&p->cwd);
	vfs_path_put(&p->root);

        spinlock_acquire(&wait_lock);

        reparent(p);

        wakeup(p->parent);

        spinlock_acquire(&p->lock);
        spinlock_acquire(&p->cur_thread->lock);

        p->exit_state = cause;
        p->state = ZOMBIE;
        p->cur_thread->state = DIED;
        
        for(i = 0; i < p->tnums; i++) {
                if(p->thread[i] && p->thread[i] != p->cur_thread) {
                        spinlock_acquire(&p->thread[i]->lock);
                        p->thread[i]->state = DIED;
                        spinlock_release(&p->thread[i]->lock);
                }
        }

        spinlock_release(&wait_lock);

        sched();
        PANIC("exit");
}

int process_wait(int target, uint64 status_address, int nohang)
{
        process_t p, pp;
	int exit_status, kids, pid;
        list_t l;

        p = cur_proc();

        spinlock_acquire(&wait_lock);

	for(;;) {
		kids = 0;
		for(l = proc.next; l != &proc; l = l->next) {
                        pp = list_entry(l, struct process, all_tag);
                        if(!pp)
                                continue;
			if(pp->parent == p &&
			   (target == -1 || target == pp->pid)) {
                                spinlock_acquire(&pp->lock);
                                kids = 1;
                                if(pp->state == ZOMBIE) {
                                        pid = pp->pid;

					exit_status =
						(pp->exit_state & 0xff) << 8;
					if(status_address &&
					   either_copyout(1, status_address,
					                  &exit_status,
					                  sizeof(exit_status)) < 0) {
						spinlock_release(&pp->lock);
						spinlock_release(&wait_lock);
						return -2;
					}

					spinlock_release(&pp->lock);
					process_free(pp);
					spinlock_release(&wait_lock);

                                        return pid;
                                }
                                spinlock_release(&pp->lock);
                        }
                }

		if(!kids || killed(p)) {
                        spinlock_release(&wait_lock);
                        return -1;
		}
		if (nohang) {
			spinlock_release(&wait_lock);
			return 0;
		}

                sleep(p, &wait_lock);
        }
}

/**
 * @description: Kill a process
 * @param {int} pid of process
 * @return {*} 0: kill successfully     1:kill failed
 * @note This process will be killed actually by user_trap_entry in trap.c
 */
int kill(int pid)
{
        process_t p;
        list_t l;

        for(l = proc.next; l != &proc; l = l->next) {
                p = list_entry(l, struct process, all_tag);
                if(!p)
                        continue;
                spinlock_acquire(&p->lock);
                if(p->pid == pid) {
                        p->killed = 1;
                        if(p->state == SLEEPING)
                                p->state = RUNNABLE;
                        spinlock_release(&p->lock);
                        return 0;
                }
                spinlock_release(&p->lock);
        }
        return -1;
}

int killed(process_t p)
{
        int killed;
        spinlock_acquire(&p->lock);
        killed = p->killed;
        spinlock_release(&p->lock);
        return killed;
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
