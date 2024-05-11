/*
 * @Author: TroyMitchell
 * @Date: 2024-04-30 06:23
 * @LastEditors: TroyMitchell
 * @LastEditTime: 2024-05-11
 * @FilePath: /caffeinix/kernel/fs/inode.c
 * @Description: This file for inode layer of file-system
 * Words are cheap so I do.
 * Copyright (c) 2024 by TroyMitchell, All Rights Reserved. 
 */
#include <inode.h>
#include <mystring.h>
#include <debug.h>
#include <balloc.h>
#include <process.h>

#define min(x1, x2)     (x1) > (x2) ? (x2) : (x1)

extern struct superblock sb;

struct inodes{
        struct spinlock lk;
        struct inode is[NINODES];
}inodes;

/**
 * @description: Inode layer of file system initialization function
 * @return {*}
 */
void iinit(void)
{
        int i = 0;

        spinlock_init(&inodes.lk, "inodes");
        for(i = 0; i < NINODES; i++) {
                sleeplock_init(&inodes.is[i].lock, "inode");
        }
}

/**
 * @description: Update the contents of the inode to disk
 * @param {inode_t} ip: pointer of inode
 * @return {*}
 */
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

/**
 * @description: Increase the number of references to inode
 * @param {inode_t} ip: pointer of inode
 * @return {*}
 */
inode_t idup(inode_t ip)
{
        spinlock_acquire(&inodes.lk);
        ip->ref ++;
        spinlock_release(&inodes.lk);
        return ip;
}

/**
 * @description: Get the physical block number on the disk through the virtual block number of the inode
 * @param {inode_t} ip: pointer of inode
 * @param {uint32} vblock: virtual block number in inode
 * @return {*} physical block number
 */
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

/**
 * @description: Clean the all content that the inode links
 * @param {inode_t} ip: pointer of inode
 * @return {*}
 */
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
        memset(ip->d.addrs, 0, NDIRECT + 1);
        ip->d.size = 0;
        iupdate(ip);
        spinlock_release(&inodes.lk);
}

/**
 * @description: Get a inode then increase reference and this function will not load data from disk
 * @param {uint32} dev: which device?
 * @param {uint32} inum: what is inum?
 * @return {*} return a inode that be not locked
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
                } else if(i->ref == 0 && empty == 0) {
                        empty = i;
                }
        }
        if(empty) {
                empty->ref = 1;
                empty->valid = 0;
                empty->dev = dev;
                empty->inum = inum;
        } else {
                PANIC("iget");
        }
        spinlock_release(&inodes.lk);
        return empty;
}

/**
 * @description: Decrease reference and clean the all content that the inode links if conditions met
 * @param {inode_t} ip: pointer of inode
 * @return {*}
 */
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

/**
 * @description: Alloc a free inode from disk and this function will writes inode to disk
 * @param {uint32} dev: which device?
 * @param {short} type: what is type of inode? 
 * @return {*} inode that be alloced
 */
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

/**
 * @description: Lock the inode so that modify it and this function will load dinode from disk if 'vaild' == 1
 * @param {inode_t} ip: pointer of inode
 * @return {*}
 */
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

/**
 * @description: Unlock inode
 * @param {inode_t} ip: pointer of inode
 * @return {*}
 */
void iunlock(inode_t ip)
{
        if(!sleeplock_holding(&ip->lock) || ip->ref < 1 || ip == 0) {
                PANIC("iunlock");
        }
        sleeplock_release(&ip->lock);
}

/**
 * @description: Unlock and put inode
 * @param {inode_t} ip: pointer of inode
 * @return {*}
 */
void iunlockput(inode_t ip)
{
        iunlock(ip);
        iput(ip);
}

/**
 * @description: Read data from inode
 * @param {inode_t*} ip: the pointer of inode
 * @param {int} user_dst: dst is a user virtual address if user_dst == 1 
 * @param {uint64} dst: address of destination
 * @param {uint32} off: offset of data in inode
 * @param {uint32} n: byte number
 * @return {*} Returns the number of bytes on success, -1 on failure.
 */
int readi(inode_t ip, int user_dst, uint64 dst, uint32 off, uint32 n)
{
        uint32 tot, rn, addr;
        bio_t b;

        /* Out of range */
        if(off > ip->d.size || off + n < off) {
                return -1;
        }
        /* Correct the size so that it does not exceed the range */
        if((off + n) > ip->d.size) {
                n = ip->d.size - off;
        }

        for(tot = 0; tot < n; tot += rn, off += rn, dst += rn) {
                addr = imap(ip, off / BSIZE);
                if(addr == 0)
                        return 0;

                b = bread(ip->dev, addr);
                if(!b) 
                        return 0;
                /* Let's determine how many bytes we need to read */
                /* 2024-05-07 ToryMitchell: Fixed a bug */
                rn = min(n - tot, BSIZE - off % BSIZE);

                if(either_copyout(user_dst, dst, b->buf + off % BSIZE, rn) == -1) {
                        brelse(b);
                        tot = -1;
                        break;
                }
                brelse(b);
        }

        return tot;
}

/**
 * @description: Write data to inode
 * @param {inode_t*} ip: the pointer of inode
 * @param {int} user_src: dst is a user virtual address if user_src == 1 
 * @param {uint64} src: address of source
 * @param {uint32} off: offset of data in inode
 * @param {uint32} n: byte number
 * @return {*} Returns the number of bytes on success, -1 on failure 
 */
int writei(inode_t ip, int user_src, uint64 src, uint32 off, uint32 n)
{
        uint32 tot, rn, addr;
        bio_t b;

        /* Out of range */
        if(off > ip->d.size || off + n < off) {
                return -1;
        }
        /* Correct the size so that it does not exceed the range */
        if((off + n) > MAXFILE*BSIZE) {
                return -1;
        }

        for(tot = 0; tot < n; tot += rn, off += rn, src += rn) {
                addr = imap(ip, off / BSIZE);
                if(addr == 0)
                        return 0;

                b = bread(ip->dev, addr);
                if(!b) 
                        return 0;
                /* Let's determine how many bytes we need to read */
                rn = min(n - tot, BSIZE - off % BSIZE);

                if(either_copyin(b->buf + off % BSIZE, user_src, src, rn) == -1) {
                        brelse(b);
                        tot = -1;
                        break;
                }
                log_write(b);
                brelse(b);
        }

        return tot;
}
