#include <log.h>
#include <debug.h>
#include <process.h>
#include <mystring.h>
#include <printf.h>

struct log_info {
        uint32 n;
        uint32 blocks[LOGSIZE];
};

struct log {
        struct spinlock lk;
        uint32 dev;
        uint32 start;
        uint32 sz;
        uint32 syscalling;
        uint8 commiting;
        struct log_info info;
}log;

static void log_head_write(void)
{
        struct log_info *info;
        bio_t binfo;

        binfo = bread(log.dev, log.start);
        info = (struct log_info*)binfo->buf;
        memmove(info, &log.info, sizeof(struct log_info));
        bwrite(binfo);
        brelse(binfo);
}

static void log_head_read(void)
{
        struct log_info *info;
        bio_t binfo;

        binfo = bread(log.dev, log.start);
        info = (struct log_info*)binfo->buf;
        memmove(&log.info, info, sizeof(struct log_info));
        brelse(binfo);
}

static void log_temp_write(void)
{
        int i;
        bio_t to,from;

        for(i = 0; i < log.info.n; i++) {
                from = bread(log.dev, log.info.blocks[i]);
                to = bread(log.dev, log.start + 1 + i);
                memmove(to->buf, from->buf, BSIZE);
                bwrite(to);
                brelse(to);
                brelse(from);
        }
}

static void log_trans(uint8 is_recover)
{
        int i;
#ifndef LOG_TEST
        bio_t to,from;
#else
        bio_t to;
#endif

        for(i = 0; i < log.info.n; i++) {
                to = bread(log.dev, log.info.blocks[i]);
#ifndef LOG_TEST
                from = bread(log.dev, log.start + 1 + i);
                memmove(to->buf, from->buf, BSIZE);
                bwrite(to);
#endif
                /* bpin in log_write */
                if(!is_recover)
                        bunpin(to);
                brelse(to);
#ifndef LOG_TEST
                brelse(from);
#endif
        }
}

static void commit(void)
{
        if(log.info.n > 0) {
                log_temp_write();
                log_head_write();
                log_trans(0);
#ifndef LOG_TEST
                spinlock_acquire(&log.lk);
                log.info.n = 0;
                spinlock_release(&log.lk);
                log_head_write();
#endif
        }
}

static void recover(void)
{
        log_head_read();
        if(log.info.n > 0) {
                log_trans(1);
                spinlock_acquire(&log.lk);
                log.info.n = 0;
                spinlock_release(&log.lk);
                log_head_write();
        }
}

void log_init(uint32 dev, uint32 sz, uint32 start)
{
        if(sizeof(log.info) > BSIZE)
                PANIC("log_init");
        spinlock_init(&log.lk, "log");

        log.sz = sz;
        log.dev = dev;
        log.start = start;
        recover();
}

void log_begin(void)
{
        /* Protect log */
        spinlock_acquire(&log.lk);
        while(1) {
                /* If it is commit-ing */
                if(log.commiting) {
                        sleep(&log, &log.lk);
                /* reserve some space for the process that will write */
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
        /* Protect log */
        spinlock_acquire(&log.lk);
        
        if(log.syscalling == 0 || log.commiting == 1) {
                PANIC("log_end");
        }

        log.syscalling --;

        if(log.syscalling == 0) {
                log.commiting = 1;
        } else {
                wakeup(&log);
        }

        spinlock_release(&log.lk);

        if(log.commiting == 1) {
                /* Commit in here */
                commit();
                spinlock_acquire(&log.lk);
                log.commiting = 0;
                wakeup(&log);
                spinlock_release(&log.lk);
        }
}

void log_write(bio_t b)
{
        int i;
        /* Protect info */
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
        /* Point the real block */
        log.info.blocks[i] = b->bnum;
        if(i == log.info.n) {
                /* Avoiding be released by brelse */
                bpin(b);
                log.info.n ++;
        }

        spinlock_release(&log.lk);
}

/*
        For test recover.
        First step: Enable the macro LOG_TEST in log.h then execute "make qemu"
        Second step: Disable the macro LOG_TEST in log.h then execute "make qemu"
        Note: You need to call this function under log_init in main.c
*/
void log_test(void)
{
        bio_t b;
#ifdef LOG_TEST
        log_begin();
        b = bread(1, 1);
        strncpy(b->buf, "test123", 8);
        log_write(b);
        brelse(b);
        log_end();
#endif
        b = bread(1, 1);
        printf("log test: %s\n", b->buf);
}
