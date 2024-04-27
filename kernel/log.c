#include <log.h>
#include <debug.h>
#include <process.h>
#include <string.h>

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

static void log_trans(void)
{
        int i;
        bio_t to,from;

        for(i = 0; i < log.info.n; i++) {
                to = bread(log.dev, log.info.blocks[i]);
                from = bread(log.dev, log.start + 1 + i);
                memmove(to->buf, from->buf, BSIZE);
                bwrite(to);
                /* bpin in log_write */
                bunpin(to);
                brelse(to);
                brelse(from);
        }
}

static void commit(void)
{
        log_temp_write();
        log_head_write();
        log_trans();
        spinlock_acquire(&log.lk);
        log.info.n = 0;
        spinlock_release(&log.lk);
        log_head_write();
}

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

