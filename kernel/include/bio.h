#ifndef __CAFFEINIX_KERNEL_BIO_H
#define __CAFFEINIX_KERNEL_BIO_H

#include <typedefs.h>
#include <sleeplock.h>
#include <virtio_disk.h>

#define BIO_NUM                         50

typedef struct bio {
        /* The content of block in disk */
        char buf[BSIZE];
        /* For task sleeps */
        struct sleeplock lk;
        uint16 dev;
        /* Block number */
        uint16 bnum;
        /* Reference count */
        uint16 ref;
        /* It has been loaded in memory? */
        uint8 vaild;
        /* For LRU */
        struct bio *prev;
        struct bio *next;
}*bio_t;

void binit(void);
bio_t bread(uint16 dev, uint16 block);
void bwrite(bio_t b);
void bpin(bio_t b);
void bunpin(bio_t b);
void brelse(bio_t b);

#endif