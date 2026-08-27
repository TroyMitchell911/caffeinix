/*
 * @Author: TroyMitchell
 * @Date: 2024-04-25 09:22
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-30
 * @FilePath: /caffeinix/kernel/include/process.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by ${TroyMitchell}, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_PROCESS_H
#define __CAFFEINIX_KERNEL_PROCESS_H

#include <thread.h>
#include <spinlock.h>
#include <riscv.h>
#include <file.h>
#include <sleeplock.h>
#include <vma.h>
#include <wait.h>
#include <signal.h>

struct tty;
struct process_vfork;

#define PROCESS_CMDLINE_MAX PGSIZE
#define PROCESS_GROUP_MAX   32

struct process_credentials {
	uint32 uid;
	uint32 euid;
	uint32 suid;
	uint32 fsuid;
	uint32 gid;
	uint32 egid;
	uint32 sgid;
	uint32 fsgid;
	uint32 group_count;
	uint32 groups[PROCESS_GROUP_MAX];
};

struct process_snapshot {
	char name[MAXNAME];
	int pid;
	int ppid;
	int pgid;
	int sid;
	int tty;
	int tty_pgid;
	char state;
	int nice;
	uint32 uid;
	uint32 euid;
	uint32 suid;
	uint32 fsuid;
	uint32 gid;
	uint32 egid;
	uint32 sgid;
	uint32 fsgid;
	uint32 group_count;
	uint32 groups[PROCESS_GROUP_MAX];
	uint32 threads;
	uint32 runnable_threads;
	uint32 blocked_threads;
	uint64 start_time_ns;
	uint64 user_time_ns;
	uint64 system_time_ns;
	uint64 children_user_time_ns;
	uint64 children_system_time_ns;
	uint64 virtual_size;
	uint64 resident_pages;
	uint64 signal_pending;
	uint64 signal_shared_pending;
	uint64 signal_blocked;
	uint64 signal_ignored;
	uint64 signal_caught;
};

struct process_system_snapshot {
	uint32 processes;
	uint32 running;
	uint32 blocked;
	int last_pid;
	uint64 total_forks;
	uint64 context_switches;
	uint64 idle_time_ns;
	uint64 user_time_ns;
	uint64 system_time_ns;
};

typedef enum process_state{
	PROCESS_EMBRYO,
        PROCESS_LIVE,
        PROCESS_ZOMBIE,
}process_state_t;

#define PROCESS_WAIT_FAULT -2
#define PROCESS_WAIT_INTR  -3

struct process_signal_action {
	uint64 handler;
	uint64 flags;
	uint64 mask;
};

typedef struct process{
        struct spinlock lock;

        char name[MAXNAME];
        int pid;
	int pgid;
	int sid;
        process_state_t state;
        
        uint64 sz;
	uint64 brk;
	uint64 brk_start;
	uint64 mmap_top;
        pagedir_t pagetable;
	struct sleeplock mmap_lock;
	struct vma_set vmas;
	struct list mmap_tag;
	uint8 mmap_registered;
	struct vfs_path root;
	struct vfs_path cwd;
	struct process_credentials credentials;
	uint32 umask;
	void *cmdline;
	uint32 cmdline_length;
	uint64 start_time_ns;
	uint64 retired_user_time_ns;
	uint64 retired_system_time_ns;
	uint64 children_user_time_ns;
	uint64 children_system_time_ns;
	struct spinlock files_lock;
        file_t ofile[NOFILE];
	uint8 fd_flags[NOFILE];
        int exit_state;
	int group_exiting;
	int group_exit_state;
	int group_exit_signal;
	int group_exit_core;
	int execing;
	int did_exec;
	int live_threads;
	int stopped;
	int child_event;
	int child_event_signal;
	uint8 auto_reap;
	uint8 membarrier_private_expedited;
	uint8 vfork_mmap_transferred;
	struct process_vfork *vfork;
	struct wait_queue vfork_wait;
	uint64 real_timer_deadline;
	uint64 real_timer_interval;
	struct signal_pending *signal_pending;
	struct process_signal_action signal_actions[64];
	struct wait_queue signal_wait;
	struct spinlock sleep_lock;
	struct wait_queue sleep_wait;
	struct tty *controlling_tty;
        struct process *parent;
	uint8 adopted_by_init;
        struct wait_queue child_wait;
	struct wait_queue thread_reap_wait;
        int tnums;
        thread_t thread[PROC_MAXTHREAD];
        
        struct list all_tag;
}*process_t;

void process_init(void);
pagedir_t process_pagedir(process_t p, thread_t thread);
void process_freepagedir(pagedir_t pgdir, uint64 sz);
int process_fork(uint64 child_stack);
int process_vfork(uint64 child_stack);
int process_clone_thread(uint64 flags, uint64 child_stack,
			 uint64 parent_tid, uint64 tls, uint64 child_tid);
void process_thread_exit(int cause, int group);
void process_signal_exit(int signal, int core_dumped);
void process_signal_stop(int signal);
void process_auto_reap(process_t process);
int process_group_exiting(process_t process, int *status);
int process_exec_begin(process_t process, thread_t thread);
int process_exec_quiesce(process_t process, thread_t thread);
void process_exec_end(process_t process, int committed);
int process_vfork_exec(process_t process);
int process_thread_exit_requested(thread_t thread, int *status);
int process_wait(int target, uint64 status_address, uint64 usage_address,
		 int options);
int process_setpgid(int pid, int pgid);
int process_getpgid(int pid);
int process_getsid(int pid);
int process_setsid(void);
struct tty *process_controlling_tty(void);
int process_tty_open(struct tty *tty, int no_ctty);
int process_tty_busy(struct tty *tty);
int process_tty_get_foreground(struct tty *tty, int *pgid);
int process_tty_set_foreground(struct tty *tty, int pgid);
int process_tty_get_session(struct tty *tty, int *sid);
int process_tty_check_read(struct tty *tty);
int process_tty_check_write(struct tty *tty, int force);
int process_tty_signal_foreground(struct tty *tty, int signal);
int process_set_nice(int pid, int nice);
int process_get_nice(int pid, int *nice);
void process_expire_timers(uint64 now);
int process_task_exists(int tid);
uint32 process_task_count(void);
int process_snapshot_pid(int pid, struct process_snapshot *snapshot,
			 char *cmdline, uint32 cmdline_size,
			 uint32 *cmdline_length);
uint32 process_snapshot_pids(int *pids, uint32 capacity);
void process_snapshot_system(struct process_system_snapshot *snapshot);
void process_set_cmdline(process_t process, void *cmdline,
			 uint32 length);
void process_credentials_get(struct process_credentials *credentials);
uint32 process_umask_get(void);
uint32 process_umask_set(uint32 mask);

int either_copyout(int user_dst, uint64 dst, void* src, uint64 len);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);
/* User init for first process */
void userinit(void);
#endif
