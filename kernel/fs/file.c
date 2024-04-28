#include <file.h>
#include <mystring.h>
#include <debug.h>

struct superblock sb;

void fs_init(uint32 dev)
{
        bio_t b;
        /* Read super block */
        b = bread(dev, SUPERBLOCK_NUM);
        memmove(&sb, b->buf, sizeof(struct superblock));
        if(sb.magic != 0x10203040)
                PANIC("fs_init");
        log_init(dev, sb.size, sb.logstart);
        brelse(b);
}