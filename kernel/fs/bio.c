#include <bio.h>
#include <block_device.h>
#include <debug.h>
#include <printf.h>

/* Remember one thing: spinlock protects information of bio and sleeplock protects buffer of bio. */

struct bio_table {
        struct spinlock lk;
        struct bio bios[BIO_NUM];
        struct bio head;
}bio_table; 

void binit(void)
{
        bio_t b;

        spinlock_init(&bio_table.lk, "bio_table");
        
        /* Initial the head */
        bio_table.head.next = bio_table.head.prev = &bio_table.head;

        for(b = bio_table.bios; b <= &bio_table.bios[BIO_NUM - 1]; b++) {
                sleeplock_init(&b->lk, "bio");
                /* Insert after the head */
                b->next = bio_table.head.next;
                b->prev = &bio_table.head;
                bio_table.head.next->prev = b;
                bio_table.head.next = b;
        }
}

static bio_t bget(uint32 dev, uint64 block)
{
        bio_t b;

        spinlock_acquire(&bio_table.lk);
        
        /* Find a bio that the bnum equals block */
        for(b = bio_table.head.next; b != &bio_table.head; b = b->next) {
                if(b->bnum == block && dev == b->dev) {
                        b->ref ++;
                        spinlock_release(&bio_table.lk);
                        sleeplock_acquire(&b->lk);
                        return b;
                }
        }

        /* LRU */
        for(b = bio_table.head.prev; b != &bio_table.head; b = b->prev) {
                if(b->ref == 0) {
                        b->dev = dev;
                        b->bnum = block;
                        b->ref = 1;
                        b->vaild = 0;


                        // /* Remove */
                        // b->next->prev = b->prev;
                        // b->prev->next = b->next;
                        // /* Insert head */
                        // b->next = bio_table.head.next;
                        // b->prev = &bio_table.head;
                        // bio_table.head.next->prev = b;
                        // bio_table.head.next = b;
                        spinlock_release(&bio_table.lk);
                        sleeplock_acquire(&b->lk);
                        return b;
                }
        }
        PANIC("bio_get");
        return 0;
}

void brelse(bio_t bio)
{
        if(!sleeplock_holding(&bio->lk)) {
                PANIC("brelse");
        }
        sleeplock_release(&bio->lk);

        spinlock_acquire(&bio_table.lk);
        if(bio->ref == 1) {
                /* Remove */
                bio->next->prev = bio->prev;
                bio->prev->next = bio->next;
                /* Insert tail */
                bio->next = &bio_table.head;
                bio->prev = bio_table.head.prev;
                bio_table.head.prev->next = bio;
                bio_table.head.prev = bio;
        }
        
        bio->ref --;
        spinlock_release(&bio_table.lk);
}

bio_t bread(uint32 dev, uint64 block)
{
        struct block_device *device;
        uint32 sectors;
        bio_t bio = bget(dev, block);

        if(bio->vaild == 0) {
                device = block_device_get(dev);
                if (!device || BSIZE % device->sector_size) {
                        brelse(bio);
                        return 0;
                }
                sectors = BSIZE / device->sector_size;
                if (block_device_read(device, block * sectors,
                                      bio->buf, sectors)) {
                        brelse(bio);
                        return 0;
                }
                bio->vaild = 1;
        }
        return bio;
}

void bwrite(bio_t bio)
{
        struct block_device *device;
        uint32 sectors;

        if(!sleeplock_holding(&bio->lk))
                PANIC("bio_write");
        device = block_device_get(bio->dev);
        if (!device || BSIZE % device->sector_size)
                PANIC("bio device");
        sectors = BSIZE / device->sector_size;
        if (block_device_write(device, bio->bnum * sectors,
                               bio->buf, sectors))
                PANIC("bio write");
}

void bpin(bio_t bio)
{
        spinlock_acquire(&bio_table.lk);
        bio->ref ++;
        spinlock_release(&bio_table.lk);
}

void bunpin(bio_t bio)
{
        spinlock_acquire(&bio_table.lk);
        bio->ref --;
        spinlock_release(&bio_table.lk);
}
