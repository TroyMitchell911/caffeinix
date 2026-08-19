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
#include <printk.h>
#include <vfs.h>
#include <linux_uapi.h>

/* From trampoline.S */
extern char trampoline[];
extern int exec_linux(char *path, char **argv, char **envp);
extern void user_trap_ret(void);

static struct spinlock wait_lock;
// struct process proc[NPROC];
struct list proc;
static process_t first;

static int setup_stdio(void)
{
	process_t p = cur_proc();
	int occupied;
	int fd;

	spinlock_acquire(&p->files_lock);
	occupied = p->ofile[0] || p->ofile[1] || p->ofile[2];
	spinlock_release(&p->files_lock);
	if (occupied)
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

static struct block_device *mount_root_device(void)
{
	struct block_device *device;
	uint32 id;

	for (id = 1; id < BLOCK_DEVICE_MAX; id++) {
		device = block_device_get(id);
		if (device &&
		    vfs_mount_root(ROOT_FILESYSTEM, device, 0) == VFS_OK)
			return device;
	}
	return 0;
}

static void mount_fat_device(struct block_device *root)
{
	struct block_device *device;
	int status;
	uint32 id;

	for (id = 1; id < BLOCK_DEVICE_MAX; id++) {
		device = block_device_get(id);
		if (!device || device == root)
			continue;
		status = vfs_mount("fat", device, "/mnt/fat", 0);
		if (status == VFS_OK)
			return;
		pr_warn("VFS: cannot mount %s as fat: %d", device->name,
			status);
	}
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
                                wait_queue_wake_all(&first->child_wait);
                                continue;
                        }  
                        spinlock_release(&pp->lock);
                }
        }
}

pagedir_t process_pagedir(process_t p, thread_t thread)
{
        int ret;
        pagedir_t pgdir;

	if (!p || !thread || thread->home != p || thread->id_p < 0 ||
	    thread->id_p >= PROC_MAXTHREAD)
                return 0;
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
	ret = vm_map(pgdir, TRAPFRAME(thread->id_p),
	             (uint64)thread->trapframe,
                     PGSIZE, PTE_W | PTE_R);
	if(ret){
                /* We don't need free the address that PTE points because it is a code seg */
		vm_unmap(pgdir, TRAMPOLINE, 1, 0);
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
	if (vm_mapped(pgdir, TRAMPOLINE))
		vm_unmap(pgdir, TRAMPOLINE, 1, 0);
	vm_free_user(pgdir);
	pagedir_free(pgdir);
}

/* This function is the first when a process first start */
static void proc_first_start(void)
{
	static uint8 fs_started;
	struct block_device *root_device;
	char *argv[] = { INIT_PATH, 0 };
	char *envp[] = {
		"HOME=/",
		"PATH=/sbin:/bin:/usr/sbin:/usr/bin",
		"TERM=vt100",
		0,
	};
	process_t process = cur_proc();

        /* The function scheduler will acquire the lock */
	spinlock_release(&cur_thread()->lock);
	if (!fs_started) {
		int status;

		fs_started = 1;
		root_device = mount_root_device();
		if (!root_device) {
			pr_err("VFS: cannot mount root filesystem %s",
				ROOT_FILESYSTEM);
			PANIC("mount root");
		}
		if (vfs_get_root(&process->root) < 0)
			PANIC("process root");
		vfs_path_copy(&process->cwd, &process->root);
		status = vfs_mount("devfs", 0, "/dev", 0);
		if (status < 0) {
			pr_err("VFS: cannot mount devfs on /dev: %d", status);
			PANIC("mount devfs");
		}
		status = vfs_mount("tmpfs", 0, "/tmp", 0);
		if (status < 0) {
			pr_err("VFS: cannot mount tmpfs on /tmp: %d", status);
			PANIC("mount tmpfs");
		}
		mount_fat_device(root_device);
		status = setup_stdio();
		if (status < 0) {
			pr_err("init: cannot configure standard I/O: %d", status);
			PANIC("stdio setup");
		}
		pr_info("init: starting %s", INIT_PATH);
		status = exec_linux(INIT_PATH, argv, envp);
		if (status < 0) {
			pr_err("init: cannot execute %s: %d", INIT_PATH, status);
			PANIC("exec init");
		}
	}
        user_trap_ret();
}

static void user_thread_start(void)
{
	spinlock_release(&cur_thread()->lock);
	user_trap_ret();
}

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
	p->state = PROCESS_LIVE;
	wait_queue_init(&p->child_wait, "child wait");
	wait_queue_init(&p->thread_reap_wait, "thread reap");
	sleeplock_init(&p->mmap_lock, "process mmap");
	spinlock_init(&p->files_lock, "process files");
	vma_set_init(&p->vmas);
        
        spinlock_init(&p->lock, "process");
        spinlock_acquire(&p->lock);

        for(i = 0; i < PROC_MAXTHREAD; i++) {
                p->thread[i] = 0;
        }

        p->tnums = 0;

        t = thread_alloc(p);
        if(!t)
                goto r0;

        /* Alloc memory for page-table */
	p->pagetable = process_pagedir(p, t);
	if(!p->pagetable) {
		goto r1;
        }
        
	/* Linux thread-group leaders share their PID and TID. */
	p->pid = t->tid;
        
        /* Set the context of return address */
        t->context.ra = (uint64)(proc_first_start);

        list_init(&p->all_tag);
        spinlock_acquire(&wait_lock);
        list_insert_after(&proc, &p->all_tag);
        spinlock_release(&wait_lock);

        return p;
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
        thread_t thread;
        int i;

	spinlock_acquire(&p->lock);
        if(!wait_queue_empty(&p->child_wait))
                PANIC("free process with child waiter");
	if (!wait_queue_empty(&p->thread_reap_wait))
		PANIC("free process with thread reaper");
	vma_set_destroy(&p->vmas);
        if(p->pagetable) {
                process_freepagedir(p->pagetable, p->sz);
        }
        for(i = 0; i < PROC_MAXTHREAD; i++) {
                thread = p->thread[i];
                if(!thread)
                        continue;
                spinlock_acquire(&thread->lock);
                thread_free(thread);
                spinlock_release(&thread->lock);
        }
        list_remove(&p->all_tag);
        spinlock_release(&p->lock);
        free(p);
        // p->pid = 0;
        // p->sz = 0;
        // p->state = UNUSED;
        // p->parent = 0;
        // p->name[0] = 0;
}

void process_init(void)
{
	/* Init the spinlock */
	spinlock_init(&wait_lock, "wait_lock");
	list_init(&proc);
}

void userinit(void)
{
        process_t p;
        thread_t t;

        /* Alloc a process */
	p = process_alloc();
	if (!p)
                PANIC("userinit");
	t = p->thread[0];
	scheduler_make_runnable(t);
	p->sz = 0;
	p->brk = 0;
	p->brk_start = 0;

	safe_strncpy(p->name, "kernel-init", MAXNAME);
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

        oldt = cur_thread();
        newt = newp->thread[0];
	scheduler_inherit(newt, oldt);

	sleeplock_acquire(&oldp->mmap_lock);
	if (vm_copy(oldp->pagetable, newp->pagetable) != 0 ||
	    vma_set_clone(&newp->vmas, &oldp->vmas) < 0) {
		sleeplock_release(&oldp->mmap_lock);
		spinlock_release(&newt->lock);
		spinlock_release(&newp->lock);
		spinlock_acquire(&wait_lock);
		process_free(newp);
		spinlock_release(&wait_lock);
		return -1;
	}
	sleeplock_release(&oldp->mmap_lock);

        newp->sz = oldp->sz;
	newp->brk = oldp->brk;
	newp->brk_start = oldp->brk_start;
	newp->umask = oldp->umask;
	*newt->trapframe = *oldt->trapframe;
	if (child_stack)
		newt->trapframe->sp = child_stack;

        pid = newp->pid;
        newt->trapframe->a0 = 0;

	spinlock_acquire(&oldp->files_lock);
	for(i = 0; i < NOFILE; i++) {
		if(oldp->ofile[i])
			newp->ofile[i] = file_dup(oldp->ofile[i]);
		newp->fd_flags[i] = oldp->fd_flags[i];
	}
	spinlock_release(&oldp->files_lock);
	vfs_path_copy(&newp->root, &oldp->root);
	vfs_path_copy(&newp->cwd, &oldp->cwd);
        /* Copy the process name into newp */
	safe_strncpy(newp->name, oldp->name, MAXNAME);
	newt->signal_mask = oldt->signal_mask;
	memmove(newp->signal_actions, oldp->signal_actions,
	        sizeof(newp->signal_actions));

        spinlock_release(&newp->lock);
        spinlock_release(&newt->lock);

        spinlock_acquire(&wait_lock);
        newp->parent = oldp;
        spinlock_release(&wait_lock);

        spinlock_acquire(&newt->lock);
        scheduler_make_runnable(newt);
        spinlock_release(&newt->lock);

        /* Return for parent process */
        return pid;
}

int process_clone_thread(uint64 flags, uint64 child_stack,
			 uint64 parent_tid, uint64 tls, uint64 child_tid)
{
	process_t p = cur_proc();
	thread_t child, current = cur_thread();
	int error = -LINUX_ENOMEM;
	int tid;

	if (!child_stack)
		return -LINUX_EINVAL;
	spinlock_acquire(&p->lock);
	if (p->execing || p->group_exiting || p->state != PROCESS_LIVE) {
		spinlock_release(&p->lock);
		return -LINUX_EAGAIN;
	}
	child = thread_alloc(p);
	if (!child) {
		spinlock_release(&p->lock);
		return -LINUX_EAGAIN;
	}
	if (vm_map(p->pagetable, TRAPFRAME(child->id_p),
	           (uint64)child->trapframe, PGSIZE, PTE_R | PTE_W) < 0)
		goto fail;

	*child->trapframe = *current->trapframe;
	child->trapframe->a0 = 0;
	child->trapframe->sp = child_stack;
	if (flags & LINUX_CLONE_SETTLS)
		child->trapframe->tp = tls;
	child->signal_mask = current->signal_mask;
	if (flags & LINUX_CLONE_CHILD_CLEARTID)
		child->clear_child_tid = child_tid;
	child->context.ra = (uint64)user_thread_start;
	safe_strncpy(child->name, current->name, sizeof(child->name));
	scheduler_inherit(child, current);
	tid = child->tid;
	if ((flags & LINUX_CLONE_PARENT_SETTID) &&
	    copyout(p->pagetable, parent_tid, (char *)&tid,
	            sizeof(tid)) < 0) {
		error = -LINUX_EFAULT;
		goto fail_mapped;
	}
	if ((flags & LINUX_CLONE_CHILD_SETTID) &&
	    copyout(p->pagetable, child_tid, (char *)&tid,
	            sizeof(tid)) < 0) {
		error = -LINUX_EFAULT;
		goto fail_mapped;
	}
	spinlock_release(&p->lock);
	scheduler_make_runnable(child);
	spinlock_release(&child->lock);
	return tid;

fail_mapped:
	vm_unmap(p->pagetable, TRAPFRAME(child->id_p), 1, 0);
fail:
	thread_free(child);
	spinlock_release(&child->lock);
	spinlock_release(&p->lock);
	return error;
}

int process_exec_begin(process_t p, thread_t current)
{
	int result = -1;

	spinlock_acquire(&p->lock);
	if (p->state == PROCESS_LIVE && !p->group_exiting && !p->execing &&
	    current->home == p) {
		p->execing = 1;
		result = 0;
	}
	spinlock_release(&p->lock);
	return result;
}

static void process_request_thread_exit_locked(thread_t thread, int status)
{
	if (!thread || !thread->home ||
	    !spinlock_holding(&thread->home->lock) ||
	    thread->state == THREAD_EXITED)
		return;
	thread->exit_status = status;
	__atomic_store_n(&thread->exit_requested, 1, __ATOMIC_RELEASE);
	wait_queue_terminate_thread(thread);
	scheduler_kick(thread);
}

int process_thread_exit_requested(thread_t thread, int *status)
{
	if (!thread ||
	    !__atomic_load_n(&thread->exit_requested, __ATOMIC_ACQUIRE))
		return 0;
	if (status)
		*status = thread->exit_status;
	return 1;
}

int process_exec_quiesce(process_t p, thread_t current)
{
	int index;

	spinlock_acquire(&p->lock);
	if (p->state != PROCESS_LIVE || p->group_exiting || !p->execing ||
	    current->home != p) {
		spinlock_release(&p->lock);
		return -1;
	}
	for (index = 0; index < PROC_MAXTHREAD; index++) {
		thread_t thread = p->thread[index];

		if (thread && thread != current)
			process_request_thread_exit_locked(thread, 0);
	}
	while (p->tnums != 1) {
		if (p->group_exiting) {
			spinlock_release(&p->lock);
			return -1;
		}
		wait_queue_sleep(&p->thread_reap_wait, &p->lock);
	}
	if (p->thread[current->id_p] != current ||
	    current->state != THREAD_RUNNING)
		PANIC("invalid exec thread");
	current->tid = p->pid;
	spinlock_release(&p->lock);
	return 0;
}

void process_exec_end(process_t p)
{
	spinlock_acquire(&p->lock);
	if (!p->execing)
		PANIC("finish inactive exec");
	p->execing = 0;
	spinlock_release(&p->lock);
}

int process_group_exiting(process_t p, int *status)
{
	int exiting;

	spinlock_acquire(&p->lock);
	exiting = p->group_exiting;
	if (exiting && status)
		*status = p->group_exit_state;
	spinlock_release(&p->lock);
	return exiting;
}

static void process_release_resources(process_t p)
{
	file_t files[NOFILE];
	int count = 0, fd;

	sleeplock_acquire(&p->mmap_lock);
	vma_set_destroy(&p->vmas);
	sleeplock_release(&p->mmap_lock);
	spinlock_acquire(&p->files_lock);
	for (fd = 0; fd < NOFILE; fd++) {
		if (!p->ofile[fd])
			continue;
		files[count++] = p->ofile[fd];
		p->ofile[fd] = 0;
		p->fd_flags[fd] = 0;
	}
	spinlock_release(&p->files_lock);
	for (fd = 0; fd < count; fd++)
		file_close(files[fd]);
	vfs_path_put(&p->cwd);
	vfs_path_put(&p->root);
}

void process_thread_exit(int cause, int group)
{
	process_t p = cur_proc();
	thread_t current = cur_thread();
	uint32 zero = 0;
	int last;
	int i;

	if (current->clear_child_tid)
		(void)copyout(p->pagetable, current->clear_child_tid,
		              (char *)&zero, sizeof(zero));

	spinlock_acquire(&p->lock);
	if (group && !p->group_exiting) {
		p->group_exiting = 1;
		p->group_exit_state = cause;
	}
	if (p->group_exiting)
		cause = p->group_exit_state;
	__atomic_store_n(&current->exit_requested, 0, __ATOMIC_RELEASE);
	if (current->state != THREAD_RUNNING || p->live_threads <= 0)
		PANIC("invalid user thread exit");
	p->live_threads--;
	last = p->live_threads == 0;
	if (p->group_exiting) {
		for (i = 0; i < PROC_MAXTHREAD; i++) {
			thread_t thread = p->thread[i];

			if (thread && thread != current)
				process_request_thread_exit_locked(
					thread, p->group_exit_state);
		}
	}
	if (!last) {
		spinlock_release(&p->lock);
		spinlock_acquire(&current->lock);
		scheduler_exit_locked();
	}
	while (p->tnums != 1)
		wait_queue_sleep(&p->thread_reap_wait, &p->lock);
	spinlock_release(&p->lock);

	process_release_resources(p);
	spinlock_acquire(&wait_lock);
	reparent(p);
	spinlock_acquire(&p->lock);
	spinlock_acquire(&current->lock);
	if (p->tnums != 1 || p->thread[current->id_p] != current ||
	    p->state != PROCESS_LIVE)
		PANIC("invalid final user thread");
	p->exit_state = cause;
	p->state = PROCESS_ZOMBIE;
	current->process_reaper = 1;
	spinlock_release(&p->lock);
	if (p->parent)
		wait_queue_wake_all(&p->parent->child_wait);
	spinlock_release(&wait_lock);
	scheduler_exit_locked();
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
				if(pp->state == PROCESS_ZOMBIE) {
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

                wait_queue_sleep(&p->child_wait, &wait_lock);
        }
}

static thread_t process_find_thread_locked(int tid, process_t *owner)
{
	process_t process;
	list_t entry;
	int index;

	if (!spinlock_holding(&wait_lock))
		PANIC("thread lookup unlocked");
	for (entry = proc.next; entry != &proc; entry = entry->next) {
		process = list_entry(entry, struct process, all_tag);
		spinlock_acquire(&process->lock);
		if (process->state != PROCESS_LIVE) {
			spinlock_release(&process->lock);
			continue;
		}
		for (index = 0; index < PROC_MAXTHREAD; index++) {
			thread_t thread = process->thread[index];

			if (thread && thread->state != THREAD_EXITED &&
			    thread->tid == tid) {
				*owner = process;
				return thread;
			}
		}
		spinlock_release(&process->lock);
	}
	return 0;
}

int process_set_nice(int tid, int nice)
{
	process_t process = 0;
	thread_t thread;
	int result = -LINUX_ESRCH;

	if (!tid)
		return scheduler_set_nice(cur_thread(), nice) < 0 ?
			-LINUX_EINVAL : 0;
	spinlock_acquire(&wait_lock);
	thread = process_find_thread_locked(tid, &process);
	if (thread)
		result = scheduler_set_nice(thread, nice) < 0 ?
			-LINUX_EINVAL : 0;
	if (process)
		spinlock_release(&process->lock);
	spinlock_release(&wait_lock);
	return result;
}

int process_get_nice(int tid, int *nice)
{
	process_t process = 0;
	thread_t thread;
	int result = -1;

	if (!nice)
		return -1;
	if (!tid) {
		*nice = scheduler_get_nice(cur_thread());
		return 0;
	}
	spinlock_acquire(&wait_lock);
	thread = process_find_thread_locked(tid, &process);
	if (thread) {
		*nice = scheduler_get_nice(thread);
		result = 0;
	}
	if (process)
		spinlock_release(&process->lock);
	spinlock_release(&wait_lock);
	return result;
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

        spinlock_acquire(&wait_lock);
        for(l = proc.next; l != &proc; l = l->next) {
                p = list_entry(l, struct process, all_tag);
                if(!p)
                        continue;
                spinlock_acquire(&p->lock);
                if(p->pid == pid) {
                        p->killed = 1;
				for(int i = 0; i < PROC_MAXTHREAD; i++)
                                if(p->thread[i])
                                        wait_queue_wake_thread(p->thread[i]);
                        spinlock_release(&p->lock);
                        spinlock_release(&wait_lock);
                        return 0;
                }
                spinlock_release(&p->lock);
        }
        spinlock_release(&wait_lock);
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
