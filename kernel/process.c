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
#include <futex.h>

#define PROCESS_CHILD_EVENT_NONE       0
#define PROCESS_CHILD_EVENT_STOPPED    1
#define PROCESS_CHILD_EVENT_CONTINUED  2

/* From trampoline.S */
extern char trampoline[];
extern char sigtrampoline[], sigtrampoline_end[];
extern int exec_linux(char *path, char **argv, char **envp);
extern void user_trap_ret(void);

static struct spinlock wait_lock;
// struct process proc[NPROC];
struct list proc;
static process_t first;

static void process_notify_parent_locked(process_t child, int code,
					 int status);

static int process_parent_auto_reaps_locked(process_t child)
{
	struct process_signal_action *action;
	process_t parent;
	int result;

	if (!spinlock_holding(&wait_lock))
		PANIC("auto reap without wait lock");
	parent = child->parent;
	if (!parent)
		return 0;
	spinlock_acquire(&parent->lock);
	action = &parent->signal_actions[LINUX_SIGCHLD - 1];
	result = action->handler == LINUX_SIG_IGN ||
	         (action->flags & LINUX_SA_NOCLDWAIT);
	spinlock_release(&parent->lock);
	return result;
}

_Static_assert(sizeof(struct signal_pending) <= PGSIZE,
	       "process signal state must fit in one page");

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
	if (sigtrampoline_end - sigtrampoline > PGSIZE ||
	    vm_map(pgdir, USER_SIGRETURN, (uint64)sigtrampoline, PGSIZE,
	           PTE_U | PTE_R | PTE_X) < 0) {
		vm_unmap(pgdir, TRAMPOLINE, 1, 0);
		pagedir_free(pgdir);
		return 0;
	}
        /* Map address of under trampoline to trapframe */
	ret = vm_map(pgdir, TRAPFRAME(thread->id_p),
	             (uint64)thread->trapframe,
                     PGSIZE, PTE_W | PTE_R);
	if(ret){
                /* We don't need free the address that PTE points because it is a code seg */
		vm_unmap(pgdir, USER_SIGRETURN, 1, 0);
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
	if (vm_mapped(pgdir, USER_SIGRETURN))
		vm_unmap(pgdir, USER_SIGRETURN, 1, 0);
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
	p->signal_pending = palloc_zero();
	if (!p->signal_pending) {
		free(p);
		return 0;
	}
	signal_process_init(p);
	p->umask = 0022;
	p->state = PROCESS_LIVE;
	wait_queue_init(&p->child_wait, "child wait");
	wait_queue_init(&p->thread_reap_wait, "thread reap");
	wait_queue_init(&p->signal_wait, "signal wait");
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
	signal_process_destroy(p);
	pfree(p->signal_pending);
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
	if (!wait_queue_empty(&p->signal_wait))
		PANIC("free process with signal waiter");
	vma_set_destroy(&p->vmas);
	signal_process_destroy(p);
	pfree(p->signal_pending);
	p->signal_pending = 0;
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
	if (vm_copy(oldp->pagetable, newp->pagetable, &oldp->vmas) != 0 ||
	    vma_set_clone(&newp->vmas, &oldp->vmas) < 0) {
		sleeplock_release(&oldp->mmap_lock);
		spinlock_release(&newt->lock);
		spinlock_release(&newp->lock);
		spinlock_acquire(&wait_lock);
		process_free(newp);
		spinlock_release(&wait_lock);
		return -1;
	}
	newp->sz = oldp->sz;
	newp->brk = oldp->brk;
	newp->brk_start = oldp->brk_start;
	sleeplock_release(&oldp->mmap_lock);

	newp->umask = oldp->umask;
	newp->membarrier_private_expedited =
		__atomic_load_n(&oldp->membarrier_private_expedited,
		                __ATOMIC_ACQUIRE);
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
	spinlock_acquire(&oldp->lock);
	signal_thread_fork(newt, oldt);
	signal_process_fork(newp, oldp);
	spinlock_release(&oldp->lock);

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

static int process_prefault_write(process_t process, uint64 address,
				  uint64 length)
{
	uint64 end, page;

	if (!length || address >= MAXVA || length > MAXVA - address)
		return -1;
	end = address + length;
	for (page = PGROUNDDOWN(address); page < end; page += PGSIZE) {
		if (mmap_handle_fault(process, page, MMAP_FAULT_WRITE) !=
		    MMAP_FAULT_OK)
			return -1;
	}
	return 0;
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
	if ((flags & LINUX_CLONE_PARENT_SETTID) &&
	    process_prefault_write(p, parent_tid, sizeof(tid)) < 0)
		return -LINUX_EFAULT;
	if ((flags & LINUX_CLONE_CHILD_SETTID) &&
	    process_prefault_write(p, child_tid, sizeof(tid)) < 0)
		return -LINUX_EFAULT;
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
	signal_thread_clone(child, current);
	if (flags & LINUX_CLONE_CHILD_CLEARTID)
		child->clear_child_tid = child_tid;
	child->context.ra = (uint64)user_thread_start;
	safe_strncpy(child->name, current->name, sizeof(child->name));
	scheduler_inherit(child, current);
	tid = child->tid;
	if ((flags & LINUX_CLONE_PARENT_SETTID) &&
	    copyout_nofault(p->pagetable, parent_tid, (char *)&tid,
	                    sizeof(tid)) < 0) {
		error = -LINUX_EFAULT;
		goto fail_mapped;
	}
	if ((flags & LINUX_CLONE_CHILD_SETTID) &&
	    copyout_nofault(p->pagetable, child_tid, (char *)&tid,
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
	if (vm_mapped(p->pagetable, USER_SIGRETURN))
		vm_unmap(p->pagetable, USER_SIGRETURN, 1, 0);
	vm_free_user(p->pagetable);
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
	int auto_reap, last;
	int i;

	futex_thread_exit(current);

	spinlock_acquire(&p->lock);
	if (group && !p->group_exiting) {
		p->group_exiting = 1;
		p->group_exit_state = cause;
		p->group_exit_signal = 0;
		p->group_exit_core = 0;
	}
	if (p->group_exiting)
		cause = p->group_exit_state;
	__atomic_store_n(&current->exit_requested, 0, __ATOMIC_RELEASE);
	if (current->state != THREAD_RUNNING || p->live_threads <= 0)
		PANIC("invalid user thread exit");
	p->live_threads--;
	signal_thread_detach_locked(p, current);
	last = p->live_threads == 0;
	if (p->group_exiting) {
		wait_queue_wake_all(&p->signal_wait);
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
	auto_reap = process_parent_auto_reaps_locked(p);
	spinlock_acquire(&p->lock);
	spinlock_acquire(&current->lock);
	if (p->tnums != 1 || p->thread[current->id_p] != current ||
	    p->state != PROCESS_LIVE)
		PANIC("invalid final user thread");
	p->exit_state = cause;
	p->state = PROCESS_ZOMBIE;
	p->auto_reap = auto_reap;
	current->process_reaper = auto_reap ? 2 : 1;
	spinlock_release(&p->lock);
	if (p->group_exit_signal)
		process_notify_parent_locked(
			p, p->group_exit_core ? LINUX_CLD_DUMPED :
			LINUX_CLD_KILLED, p->group_exit_signal);
	else
		process_notify_parent_locked(p, LINUX_CLD_EXITED,
		                             p->exit_state & 0xff);
	spinlock_release(&wait_lock);
	scheduler_exit_locked();
}

int process_wait(int target, uint64 status_address, int options)
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
				if (pp->auto_reap) {
					spinlock_release(&pp->lock);
					continue;
				}
                                kids = 1;
				if(pp->state == PROCESS_ZOMBIE) {
                                        pid = pp->pid;

					if (pp->group_exit_signal)
						exit_status =
							(pp->group_exit_signal & 0x7f) |
							(pp->group_exit_core ? 0x80 : 0);
					else
						exit_status =
							(pp->exit_state & 0xff) << 8;
					if(status_address &&
					   either_copyout(1, status_address,
					                  &exit_status,
					                  sizeof(exit_status)) < 0) {
						spinlock_release(&pp->lock);
						spinlock_release(&wait_lock);
						return PROCESS_WAIT_FAULT;
					}

					spinlock_release(&pp->lock);
					process_free(pp);
					spinlock_release(&wait_lock);

                                        return pid;
                                }
				if ((pp->child_event ==
				     PROCESS_CHILD_EVENT_STOPPED &&
				     (options & LINUX_WUNTRACED)) ||
				    (pp->child_event ==
				     PROCESS_CHILD_EVENT_CONTINUED &&
				     (options & LINUX_WCONTINUED))) {
					pid = pp->pid;
					if (pp->child_event ==
					    PROCESS_CHILD_EVENT_STOPPED)
						exit_status =
							(pp->child_event_signal << 8) |
							0x7f;
					else
						exit_status = 0xffff;
					if (status_address &&
					    either_copyout(1, status_address,
					                   &exit_status,
					                   sizeof(exit_status)) < 0) {
						spinlock_release(&pp->lock);
						spinlock_release(&wait_lock);
						return PROCESS_WAIT_FAULT;
					}
					pp->child_event =
						PROCESS_CHILD_EVENT_NONE;
					spinlock_release(&pp->lock);
					spinlock_release(&wait_lock);
					return pid;
				}
                                spinlock_release(&pp->lock);
                        }
                }

		if(!kids) {
                        spinlock_release(&wait_lock);
                        return -1;
		}
		if (options & LINUX_WNOHANG) {
			spinlock_release(&wait_lock);
			return 0;
		}
		if (signal_pending_unblocked(cur_thread())) {
			spinlock_release(&wait_lock);
			return PROCESS_WAIT_INTR;
		}

		if (wait_queue_sleep_interruptible(&p->child_wait,
		                                   &wait_lock) ==
		    WAIT_QUEUE_INTERRUPTED)
			continue;
        }
}

void process_auto_reap(process_t process)
{
	if (!process)
		PANIC("auto reap null process");
	spinlock_acquire(&wait_lock);
	spinlock_acquire(&process->lock);
	if (process->state != PROCESS_ZOMBIE || !process->auto_reap ||
	    process->tnums != 1) {
		spinlock_release(&process->lock);
		spinlock_release(&wait_lock);
		PANIC("invalid auto reap process");
	}
	spinlock_release(&process->lock);
	process_free(process);
	spinlock_release(&wait_lock);
}

static process_t process_find_locked(int pid)
{
	process_t process;
	list_t entry;

	if (!spinlock_holding(&wait_lock))
		PANIC("process lookup unlocked");
	for (entry = proc.next; entry != &proc; entry = entry->next) {
		process = list_entry(entry, struct process, all_tag);
		if (process->pid == pid && process->state == PROCESS_LIVE)
			return process;
	}
	return 0;
}

static void process_notify_parent_locked(process_t child, int code,
					 int status)
{
	process_t parent = child->parent;
	struct signal_info information = {
		.signal = LINUX_SIGCHLD,
		.code = code,
		.sender_pid = child->pid,
		.sender_uid = 0,
		.status = status,
	};
	int suppress;

	if (!spinlock_holding(&wait_lock) || !parent)
		return;
	spinlock_acquire(&parent->lock);
	suppress = (code == LINUX_CLD_STOPPED ||
	            code == LINUX_CLD_CONTINUED) &&
		(parent->signal_actions[LINUX_SIGCHLD - 1].flags &
		 LINUX_SA_NOCLDSTOP);
	if (!suppress)
		(void)signal_queue_process_locked(parent, LINUX_SIGCHLD,
		                                  &information);
	spinlock_release(&parent->lock);
	wait_queue_wake_all(&parent->child_wait);
}

static void process_continue_event_locked(process_t process, int resumed)
{
	if (!resumed)
		return;
	spinlock_acquire(&process->lock);
	process->child_event = PROCESS_CHILD_EVENT_CONTINUED;
	process->child_event_signal = LINUX_SIGCONT;
	spinlock_release(&process->lock);
	process_notify_parent_locked(process, LINUX_CLD_CONTINUED,
	                             LINUX_SIGCONT);
}

int signal_send_process(int pid, int signal,
			const struct signal_info *information)
{
	process_t process;
	int result = -1, resumed = 0;

	spinlock_acquire(&wait_lock);
	process = process_find_locked(pid);
	if (process) {
		spinlock_acquire(&process->lock);
		if (!signal)
			result = 0;
		else
			result = resumed = signal_queue_process_locked(
				process, signal, information);
		spinlock_release(&process->lock);
		if (result >= 0)
			process_continue_event_locked(process, resumed);
	}
	spinlock_release(&wait_lock);
	return result < 0 ? result : 0;
}

int signal_send_thread(int thread_group, int tid, int signal,
		       const struct signal_info *information)
{
	process_t process;
	thread_t target = 0;
	list_t entry;
	int index, result = -1, resumed = 0;

	spinlock_acquire(&wait_lock);
	for (entry = proc.next; entry != &proc; entry = entry->next) {
		process = list_entry(entry, struct process, all_tag);
		if (process->state != PROCESS_LIVE ||
		    (thread_group && process->pid != thread_group))
			continue;
		spinlock_acquire(&process->lock);
		for (index = 0; index < PROC_MAXTHREAD; index++) {
			target = process->thread[index];
			if (target && target->tid == tid &&
			    target->state != THREAD_EXITED)
				break;
			target = 0;
		}
		if (!target) {
			spinlock_release(&process->lock);
			continue;
		}
		if (!signal)
			result = 0;
		else
			result = resumed = signal_queue_thread_locked(
				process, target, signal, information);
		spinlock_release(&process->lock);
		if (result >= 0)
			process_continue_event_locked(process, resumed);
		break;
	}
	spinlock_release(&wait_lock);
	return result < 0 ? result : 0;
}

void process_signal_exit(int signal, int core_dumped)
{
	process_t process = cur_proc();

	spinlock_acquire(&process->lock);
	if (!process->group_exiting) {
		process->group_exiting = 1;
		process->group_exit_state = 0;
		process->group_exit_signal = signal;
		process->group_exit_core = !!core_dumped;
	}
	process->stopped = 0;
	wait_queue_wake_all(&process->signal_wait);
	spinlock_release(&process->lock);
	process_thread_exit(0, 0);
}

void process_signal_stop(int signal)
{
	process_t process = cur_proc();
	int notify = 0;

	spinlock_acquire(&wait_lock);
	spinlock_acquire(&process->lock);
	if (!process->stopped && !process->group_exiting) {
		process->stopped = 1;
		process->child_event = PROCESS_CHILD_EVENT_STOPPED;
		process->child_event_signal = signal;
		notify = 1;
	}
	spinlock_release(&process->lock);
	if (notify)
		process_notify_parent_locked(process, LINUX_CLD_STOPPED,
		                             signal);
	spinlock_release(&wait_lock);

	spinlock_acquire(&process->lock);
	while (process->stopped && !process->group_exiting)
		wait_queue_sleep(&process->signal_wait, &process->lock);
	spinlock_release(&process->lock);
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
