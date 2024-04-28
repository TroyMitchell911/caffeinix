#include <inode.h>
#include <mystring.h>
#include <debug.h>

extern struct superblock sb;

struct inodes{
        struct spinlock lk;
        struct inode is[NINODES];
}inodes;

void iinit(void)
{
        int i = 0;

        spinlock_init(&inodes.lk, "inodes");
        for(i = 0; i < NINODES; i++) {
                sleeplock_init(&inodes.is[i].lock, "inode");
        }
}

inode_t iget(uint32 dev, uint32 inum)
{
        inode_t i, empty = 0;
        spinlock_acquire(&inodes.lk);
        for(i = &inodes.is[0]; i < &inodes.is[NINODES - 1]; i ++) {
                if(i->ref > 0 && i->dev == dev && i->inum == inum) {
                        /* If the reference of inode greater than 0 and some value equal */
                        i->ref ++;
                        spinlock_release(&inodes.lk);
                        return i;
                } else if(i->ref == 0 && !empty) {
                        empty = i;
                }
        }
        if(!empty) {
                i->ref = 1;
                i->valid = 0;
                i->dev = dev;
                i->inum = inum;
        } else {
                PANIC("iget");
        }
        spinlock_release(&inodes.lk);
        return 0;
}

inode_t ialloc(uint32 dev, short type)
{
        int i;
        bio_t b;
        dinode_t di;

        for(i = ROOTINO; i < sb.ninodes; i++) {
                b = bread(dev, sb.inodestart + i / IPB);
                di = ((dinode_t)b->buf) + i % IPB;
                if(di->type == 0) {
                        memset(di, 0, sizeof(struct dinode));
                        di->type = type;
                        log_write(b);
                        brelse(b);
                        return iget(dev, i);
                }
                brelse(b);
        }
        PANIC("ialloc");
        return 0;
}

