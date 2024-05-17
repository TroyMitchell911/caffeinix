/*
 * @Author: TroyMitchell
 * @Date: 2024-05-16
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-17
 * @FilePath: /caffeinix/kernel/include/thread.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_THREAD_H
#define __CAFFEINIX_KERNEL_THREAD_H

#include <typedefs.h>
#include <list.h>
#include <spinlock.h>
#include <kernel_config.h>
#include <riscv.h>

typedef struct process *process_t;

typedef enum thread_state {
        NUSED,
        NREADY,
        READY,
        RESETING,
        ACTIVE,
}thread_state_t;

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

typedef struct trapframe {
        /* kernel page table */
        /*   0 */ uint64 kernel_satp; 
        /* top of process's kernel stack */
        /*   8 */ uint64 kernel_sp; 
        /* usertrap() */   
        /*  16 */ uint64 kernel_trap; 
        /* saved user program counter */  
        /*  24 */ uint64 epc; 
        /* saved kernel tp */      
        /*  32 */ uint64 kernel_hartid; 
        /*  40 */ uint64 ra;
        /*  48 */ uint64 sp;
        /*  56 */ uint64 gp;
        /*  64 */ uint64 tp;
        /*  72 */ uint64 t0;
        /*  80 */ uint64 t1;
        /*  88 */ uint64 t2;
        /*  96 */ uint64 s0;
        /* 104 */ uint64 s1;
        /* 112 */ uint64 a0;
        /* 120 */ uint64 a1;
        /* 128 */ uint64 a2;
        /* 136 */ uint64 a3;
        /* 144 */ uint64 a4;
        /* 152 */ uint64 a5;
        /* 160 */ uint64 a6;
        /* 168 */ uint64 a7;
        /* 176 */ uint64 s2;
        /* 184 */ uint64 s3;
        /* 192 */ uint64 s4;
        /* 200 */ uint64 s5;
        /* 208 */ uint64 s6;
        /* 216 */ uint64 s7;
        /* 224 */ uint64 s8;
        /* 232 */ uint64 s9;
        /* 240 */ uint64 s10;
        /* 248 */ uint64 s11;
        /* 256 */ uint64 t3;
        /* 264 */ uint64 t4;
        /* 272 */ uint64 t5;
        /* 280 */ uint64 t6;
}*trapframe_t;

typedef struct thread {
        char name[MAXNAME];
        struct spinlock lock;

        int tid;
        int id_p;

        thread_state_t state;

        uint64 kstack;
        trapframe_t trapframe;
        struct context context;
        
        void *sleep_chan;
        process_t home;
        struct list all_tag;
}*thread_t;

extern struct thread thread[NTHREAD];

typedef void (*thread_func_t)(void*);

void map_kernel_stack(pagedir_t pgdir);

void thread_setup(void);
thread_t thread_alloc(process_t p);
void thread_free(thread_t t);

#endif