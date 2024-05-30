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

/* TODO:Delete this macro */
// #define PROCESS_NO_SCHED                1

typedef struct inode *inode_t;

typedef enum process_state{
        UNUSED,
        ALLOCED,
        RUNNABLE,
        RUNNING,
        SLEEPING,
        ZOMBIE,
}process_state_t;

typedef struct trapframe_info {
        uint64 addr;
        uint64 nums;
}*trapframe_info_t;

typedef struct process{
        struct spinlock lock;

        char name[MAXNAME];
        int pid;
        process_state_t state;
        
        uint64 sz;
        pagedir_t pagetable;
        inode_t cwd;
        char cwd_name[MAXPATH];
        file_t ofile[NOFILE];
        void *sleep_chan;

        int exit_state;
        int killed;
        struct process *parent;
        trapframe_info_t tinfo;
        int tnums;
        thread_t thread[PROC_MAXTHREAD];
        thread_t cur_thread;
        
        struct list all_tag;
}*process_t;

void process_init(void);
pagedir_t process_pagedir(process_t p);
void process_freepagedir(pagedir_t pgdir, uint64 sz);
int process_grow(int n);

int killed(process_t p);
void sleep(void* chan, spinlock_t lk);
void wakeup(void* chan);
void sleep_(void* chan, spinlock_t lk);
void wakeup_(void* chan);
int either_copyout(int user_dst, uint64 dst, void* src, uint64 len);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);
/* User init for first process */
void userinit(void);
#endif