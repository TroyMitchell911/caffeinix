/*
 * @Author: TroyMitchell
 * @Date: 2024-05-16
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-18
 * @FilePath: /caffeinix/kernel/include/thread.h
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#ifndef __CAFFEINIX_KERNEL_THREAD_H
#define __CAFFEINIX_KERNEL_THREAD_H

#include <typedefs.h>
#include <list.h>
#include <rbtree.h>
#include <spinlock.h>
#include <kernel_config.h>
#include <riscv.h>
#include <signal.h>

typedef struct process *process_t;
struct wait_queue;
typedef void (*thread_func_t)(void *);

typedef enum thread_state {
        THREAD_UNUSED,
        THREAD_ALLOCATED,
        THREAD_RUNNABLE,
        THREAD_RUNNING,
        THREAD_SLEEPING,
        THREAD_EXITED,
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
	/* 288 */ uint64 f[32];
	/* 544 */ uint64 fcsr;
}*trapframe_t;

struct sched_entity {
	struct rb_node run_node;
	uint64 vruntime;
	uint64 exec_start;
	uint64 sum_exec_runtime;
	uint64 slice_ns;
	uint32 weight;
	int8 nice;
	uint8 initialized;
	uint8 on_runqueue;
};

typedef struct thread {
        char name[MAXNAME];
        struct spinlock lock;

        int tid;
        int id_p;

        thread_state_t state;

        uint64 kstack;
        trapframe_t trapframe;
        struct context context;

        process_t home;
	uint64 clear_child_tid;
	uint64 robust_list;
	uint64 robust_list_len;
	uint64 signal_mask;
	struct signal_pending signal_pending;
	uint64 signal_altstack_sp;
	uint64 signal_altstack_size;
	uint32 signal_altstack_flags;
	uint64 signal_saved_mask;
	uint64 syscall_a0;
	uint8 syscall_restart;
	uint8 futex_restart_active;
	uint8 futex_restart_armed;
	uint8 futex_restart_resume;
	uint8 futex_restart_private;
	uint64 futex_restart_epc;
	uint64 futex_restart_address;
	uint64 futex_restart_timeout_address;
	uint64 futex_restart_deadline;
	uint32 futex_restart_expected;
	uint32 futex_restart_bitset;
	uint64 signal_sequence;
	uint64 process_signal_target;
	uint8 signal_restore_mask;
	uint8 process_reaper;
	uint8 exit_requested;
	int exit_status;
	thread_func_t kernel_function;
	void *kernel_argument;
	uint8 kernel_thread;
	int lwip_errno;
	struct sched_entity sched;
        struct wait_queue *waiting_on;
        struct list wait_node;
        uint8 on_waitqueue;
	struct list timeout_node;
	uint64 wait_deadline;
	int wait_result;
	uint8 on_timeout_queue;
	uint8 wait_interruptible;
	void *wait_private;
	uint32 wait_bitset;
}*thread_t;

extern struct thread thread[NTHREAD];

void map_kernel_stack(pagedir_t pgdir);

void thread_setup(void);
thread_t thread_alloc(process_t p);
void thread_free(thread_t t);
void user_thread_reap(thread_t t);
int thread_get_robust_list(int tid, uint64 *head, uint64 *length);
int thread_last_user_tid(void);
thread_t kernel_thread_create(const char *name, thread_func_t function,
			      void *argument);
void kernel_thread_reap(thread_t thread);

#endif
