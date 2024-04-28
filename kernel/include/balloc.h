#ifndef __CAFFEINIX_KERNEL_FS_BALLOC_H
#define __CAFFEINIX_KERNEL_FS_BALLOC_H

#include <log.h>

/* How many bits per block : 8096 bits */
#define BPB                  (1024*8)

void bzero(uint32 dev, uint32 block);
uint32 balloc(uint32 dev);
void bfree(uint32 dev, uint32 block);

#endif