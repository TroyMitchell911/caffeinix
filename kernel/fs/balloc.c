#include <balloc.h>
#include <file.h>
#include <debug.h>
#include <mystring.h>

extern struct superblock sb;

void bzero(uint32 dev, uint32 block)
{
        bio_t b;
        b = bread(dev, block);
        memset(b->buf, 0, BSIZE);
        log_write(b);
        brelse(b);
}

uint32 balloc(uint32 dev)
{
        int i,j;
        uint8 mask;
        bio_t b;
        /* Traverse the bitmap blocks corresponding to all blocks */
        for(i = 0; i < sb.size; i += BPB) {
                b = bread(dev, BBLOCK(i, sb));
                /* Iterate through each bit of the bitmap block */
                for(j = 0; j < BPB; j++) {
                        mask = 1 << (j % 8);
                        if((b->buf[j / 8] & mask) == 0) {
                                b->buf[j / 8] |= mask;
                                log_write(b);
                                brelse(b);
                                bzero(dev, i + j);
                                return i + j;
                        }
                }
                brelse(b);
        }
        
        PANIC("balloc");
        return 0;
}

void bfree(uint32 dev, uint32 block)
{
        uint32 bitmap_bit = block % BPB;
        uint8 mask = 1 << (bitmap_bit % 8);
        bio_t b;

        b = bread(dev, BBLOCK(block, sb));

        if((b->buf[bitmap_bit / 8] & mask) == 0)
                PANIC("bfree");
        
        b->buf[bitmap_bit / 8] &= ~mask;
        log_write(b);
        brelse(b);
}
