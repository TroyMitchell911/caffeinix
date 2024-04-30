#include <inode.h>
#include <mystring.h>
#include <debug.h>
#include <balloc.h>

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

void iupdate(inode_t ip)
{
        bio_t b;
        dinode_t dinode;
        b = bread(ip->dev, IBLOCK(ip->inum, sb));
        /* Which one inode */
        dinode = ((dinode_t)b->buf) + ip->inum % IPB;
        /* Move the memory */
        memmove(dinode, &ip->d, sizeof(struct dinode));
        log_write(b);
        brelse(b);
}

inode_t idup(inode_t ip)
{
        spinlock_acquire(&inodes.lk);
        ip->ref ++;
        spinlock_release(&inodes.lk);
        return ip;
}

/* Return a real block number */
uint32 imap(inode_t ip, uint32 vblock)
{
        uint32 pblock, *indirect;
        bio_t b;
        if(vblock < NDIRECT) {
                /* It's a direct block */
                pblock = ip->d.addrs[vblock];
                /* If this space of virtual block is not used */
                if(pblock == 0) {
                        /* Alloc a block */
                        pblock = balloc(ip->dev);
                        ip->d.addrs[vblock] = pblock;
                        return 0;
                }
                /* return the physical block number */
                return pblock;
        }
        vblock -= NDIRECT;
        if(vblock < NINDIRECT) {
                if(ip->d.addrs[NDIRECT] == 0) {
                        ip->d.addrs[NDIRECT] = balloc(ip->dev);
                }
                b = bread(ip->dev, ip->d.addrs[NDIRECT]);
                indirect = (uint32*)b->buf;
                
                pblock = indirect[vblock];
                if(pblock == 0) {
                        /* Alloc a block */
                        pblock = balloc(ip->dev);
                        indirect[vblock] = pblock;
                        log_write(b);
                }
                brelse(b);
                return pblock;
        }
        return 0;
}

void itrunc(inode_t ip)
{
        uint32 i, *addr;
        bio_t b;
        if(ip == 0)
                PANIC("itrunc");

        spinlock_acquire(&inodes.lk);

        for(i = 0; i < NDIRECT; i++) {
                if(ip->d.addrs[i] != 0) {
                        bfree(ip->dev, ip->d.addrs[i]);
                }
        }
        if(ip->d.addrs[NDIRECT] != 0) {
                b = bread(ip->dev, ip->d.addrs[NDIRECT]);
                addr = (uint32*)b->buf;
                for(i = 0; i < NINDIRECT; i++) {
                        if(addr[i] != 0) {
                                bfree(ip->dev, addr[i]);
                        }
                }
                brelse(b);
                bfree(ip->dev, ip->d.addrs[NDIRECT]);
        }
        memmove(ip->d.addrs, 0, NDIRECT + 1);
        ip->d.size = 0;
        spinlock_release(&inodes.lk);
}

/* 
        This function will return a inode that be not locked.
        At same time, this function will not load data from disk.
*/
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

void iput(inode_t ip)
{
        if(ip->ref < 1)
                PANIC("iput");
        spinlock_acquire(&inodes.lk);
        /* We should release all block that the file used */
        if(ip->valid && ip->ref == 1 && ip->d.nlink == 0) {
                spinlock_release(&inodes.lk);
                /* There is not other process gets the inode if the ip->ref == 1 */
                sleeplock_acquire(&ip->lock);
                /* Clear the block that the file used */
                itrunc(ip);
                ip->d.type = 0;
                iupdate(ip);
                ip->valid = 0;
                sleeplock_release(&ip->lock);

                spinlock_acquire(&inodes.lk);
        }
        ip->ref --;
        spinlock_release(&inodes.lk);
}


inode_t ialloc(uint32 dev, short type)
{
        int i;
        bio_t b;
        dinode_t di;

        for(i = ROOTINO; i < sb.ninodes; i++) {
                b = bread(dev, IBLOCK(i, sb));
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

void ilock(inode_t ip)
{
        bio_t b;
        dinode_t dip;
        sleeplock_acquire(&ip->lock);

        if(!ip->valid) {
                b = bread(ip->dev, IBLOCK(ip->inum, sb));
                dip = ((dinode_t)b->buf) + ip->inum % IPB;
                memmove(&ip->d, dip, sizeof(struct dinode));
                brelse(b);
                ip->valid = 1;
                if(ip->d.type == 0) {
                        PANIC("ilock");
                }
        }
}

void iunlock(inode_t ip)
{
        if(!sleeplock_holding(&ip->lock) || ip->ref < 1 || ip == 0) {
                PANIC("iunlock");
        }
        sleeplock_release(&ip->lock);
}

void iunlockput(inode_t ip)
{
        iunlock(ip);
        iput(ip);
}


