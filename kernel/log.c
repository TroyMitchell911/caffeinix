#include <log.h>
#include <debug.h>
#include <process.h>

struct log_info {
        uint16 n;
        uint16 blocks[LOGSIZE];
};

struct log {
        struct spinlock lk;
        uint16 dev;
        uint16 start;
        uint16 sz;
        uint16 syscalling;
        uint8 commiting;
        struct log_info info;
}log;

void log_init(uint16 dev)
{
        if(sizeof(log.info) > BSIZE)
                PANIC("log_init");
        spinlock_init(&log.lk, "log");

        log.sz = LOGSIZE;
        log.dev = 1;
        log.start = 2;
        log.info.n = 0;
}

void log_begin(void)
{
        spinlock_acquire(&log.lk);
        while(1) {
                if(log.commiting) {
                        sleep(&log, &log.lk);
                } else if(log.info.n + (log.syscalling + 1)* LOGOP > log.sz) {
                        sleep(&log, &log.lk);
                } else {
                        log.syscalling++;
                        break;
                }
        }
        spinlock_release(&log.lk);
}

void log_end(void)
{
        spinlock_acquire(&log.lk);

        if(log.syscalling == 0 || log.commiting == 1) {
                PANIC("log_end");
        }

        log.syscalling --;

        if(log.syscalling == 1) {
                log.commiting = 1;
        } else {
                wakeup(&log);
        }

        spinlock_release(&log.lk);

        if(log.commiting == 0) {
                /* Commit in here */
                spinlock_acquire(&log.lk);
                log.commiting = 0;
                spinlock_release(&log.lk);
        }
}

void log_write(bio_t b)
{
        int i;
        
        spinlock_acquire(&log.lk);
        if(log.info.n > log.sz || log.info.n > LOGSIZE) {
                PANIC("log_write I'm full");
        }

        if(log.syscalling < 1) {
                PANIC("log_write Where is log_begin?");
        }

        for(i = 0; i < log.info.n; i++) {
                if(log.info.blocks[i] == b->bnum)
                        break;
        }

        log.info.blocks[i] = b->bnum;
        if(i == log.info.n) {
                bpin(b);
                log.info.n ++;
        }

        spinlock_release(&log.lk);
}

