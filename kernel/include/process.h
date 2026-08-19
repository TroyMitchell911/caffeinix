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

typedef enum process_state{
        PROCESS_LIVE,
        PROCESS_ZOMBIE,
}process_state_t;

struct process_signal_action {
	uint64 handler;
	uint64 flags;
	uint64 mask;
};

typedef struct process{
        struct spinlock lock;

        char name[MAXNAME];
        int pid;
        process_state_t state;
        
        uint64 sz;
	uint64 brk;
	uint64 brk_start;
        pagedir_t pagetable;
	struct sleeplock mmap_lock;
	struct vma_set vmas;
	struct vfs_path root;
	struct vfs_path cwd;
	uint32 umask;
	struct spinlock files_lock;
        file_t ofile[NOFILE];
	uint8 fd_flags[NOFILE];
        int exit_state;
        int killed;
	int group_exiting;
	int group_exit_state;
	int execing;
	int live_threads;
	struct process_signal_action signal_actions[64];
        struct process *parent;
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
int process_clone_thread(uint64 flags, uint64 child_stack,
			 uint64 parent_tid, uint64 tls, uint64 child_tid);
void process_thread_exit(int cause, int group);
int process_group_exiting(process_t process, int *status);
int process_exec_begin(process_t process, thread_t thread);
int process_exec_quiesce(process_t process, thread_t thread);
void process_exec_end(process_t process);
int process_thread_exit_requested(thread_t thread, int *status);
int process_wait(int target, uint64 status_address, int nohang);
int process_set_nice(int pid, int nice);
int process_get_nice(int pid, int *nice);

int killed(process_t p);
int either_copyout(int user_dst, uint64 dst, void* src, uint64 len);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);
/* User init for first process */
void userinit(void);
#endif
