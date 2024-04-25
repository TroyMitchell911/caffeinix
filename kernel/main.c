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

volatile static uint8 start = 0;
extern char end[];
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
                thread_init();
                trap_init_lock();
                trap_init();
                process_init();
                userinit();
                virtio_disk_init();
                char buf1[1024] = {"test"};
                char buf2[1024] = {0};
                virtio_disk_rw(buf1, 0, 1);
                virtio_disk_rw(buf2, 0, 0);
                printf("virtio_disk: %s\n", buf2);
                

                printf("Hello! Caffeinix\n");

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