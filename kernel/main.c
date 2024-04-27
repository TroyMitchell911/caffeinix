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
#include <string.h>
#include <log.h>

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
                binit();
                log_init(1);

                intr_on();

                log_begin();
                bio_t b = bread(1, 1);
                strncpy(b->buf, "test1", 6);
                log_write(b);
                brelse(b);
                log_end();
                b = bread(1, 1);
                printf("log test: %s\n", b->buf);

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
        // scheduler();

        while(1);
}