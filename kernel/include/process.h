/*
 * @Author: TroyMitchell
 * @Date: 2024-04-25 09:22
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-15
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

#define MAXTHREAD                       3
#define MAXNAME                         16

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


typedef struct context {
        uint64 ra;
        uint64 sp;

        /* Callee saved */
        uint64 s0;
        uint64 s1;
        uint64 s2;
        uint64 s3;
        uint64 s4;
        uint64 s5;
        uint64 s6;
        uint64 s7;
        uint64 s8;
        uint64 s9;
        uint64 s10;
        uint64 s11;
}*context_t;

typedef struct trapframe_info {
        uint64 addr;
        uint64 nums;
}*trapframe_info_t;

typedef struct process{
        struct spinlock lock;

        char name[MAXNAME];
        int pid;
        process_state_t state;
        
        uint64 kstack;
        uint64 sz;
        pagedir_t pagetable;
        inode_t cwd;
        file_t ofile[NOFILE];
        void *sleep_chan;

        struct process *parent;
        struct context context;
        trapframe_info_t tinfo;
        struct thread thread[MAXTHREAD];
        thread_t cur_thread;
}*process_t;

void process_map_kernel_stack(pagedir_t pgdir);
void process_init(void);
pagedir_t process_pagedir(process_t p);
void process_freepagedir(pagedir_t pgdir, uint64 sz);

void sleep(void* chan, spinlock_t lk);
void wakeup(void* chan);
void sleep_(void* chan, spinlock_t lk);
void wakeup_(void* chan);
int either_copyout(int user_dst, uint64 dst, void* src, uint64 len);
int either_copyin(void *dst, int user_src, uint64 src, uint64 len);
/* User init for first process */
void userinit(void);
#endif