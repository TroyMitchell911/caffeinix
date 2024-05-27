/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-27
 * @FilePath: /caffeinix/kernel/main.c
 * @Description: 
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <mem_layout.h>
#include <vm.h>
#include <console.h>
#include <palloc.h>
#include <thread.h>
#include <scheduler.h>
#include <trap.h>
#include <printf.h>
#include <plic.h>
#include <process.h>
#include <virtio_disk.h>
#include <bio.h>
#include <log.h>
#include <inode.h>
#include <file.h>
#include <mystring.h>

volatile static uint8 start = 0;
extern char end[];

static void mem_print(int *a, int sz)
{
        for(int i = 0; i < sz; i++) {
                printf("%d ", a[i]);
        }
        printf("\n");
}

static void mem_test(void)
{
        int *test1 = malloc(10 * sizeof(int));

        for(int i = 0; i < 10; i++) {
                test1[i] = i;
        }
        
        int *test2 = malloc(10 * sizeof(int));
        for(int i = 0; i < 10; i++) {
                test2[i] = 10 + i;
        }

        printf("test1:%p test2:%p\n", test1, test2);

        mem_print(test1, 10);
        mem_print(test2, 10);
}

void main(void)
{
        if(cpuid() == 0) {
                palloc_init();
                console_init();
                printf_init();
                plic_init();
                plic_init_hart();
                printf("%p\n", end);
                kvm_create();
                kvm_init();
                thread_setup();
                trap_init_lock();
                trap_init();
                process_init();
                userinit();
                binit();
                iinit();
                file_init();
                virtio_disk_init();
                
                char *test = malloc(strlen("Hello! Caffeinix\n") + 1);
                printf("test: %p\n", test);
                strncpy(test, "Hello! Caffeinix\n", strlen("Hello! Caffeinix\n") + 1);
                printf(test);
                free(test);
                mem_test();
                // thread_test();
                
                __sync_synchronize();
                start = 1;
                
        } else {
                while(start == 0)
                        ;
                __sync_synchronize();
                kvm_init();
                plic_init_hart();
                trap_init();
        }

        printf("hardid %d started\n", cpuid());
        scheduler();

        while(1);
}