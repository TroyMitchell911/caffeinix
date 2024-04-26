#include <bio.h>
#include <debug.h>

struct bio_table {
        struct spinlock lk;
        struct bio bios[BIO_NUM];
        struct bio head;
}bio_table;

void bio_init(void)
{
        bio_t b;

        spinlock_init(&bio_table.lk, "bio_table");
        
        /* Initial the head */
        b = &bio_table.head;
        b->next = b->prev = b;

        for(b = bio_table.bios; b <= &bio_table.bios[BIO_NUM - 1]; b++) {
                sleeplock_init(&b->lk, "bio");
                /* Insert after the head */
                b->next = bio_table.head.next;
                b->prev = &bio_table.head;
                bio_table.head.next->prev = b;
                bio_table.head.next = b;
                b->ref = b->vaild = 0;
        }
}

bio_t bio_get(uint16 dev, uint16 block)
{
        bio_t b;

        spinlock_acquire(&bio_table.lk);
        
        /* Find a bio that the bnum equals block */
        for(b = bio_table.head.next; b->next != &bio_table.head; b = b->next) {
                if(b->bnum == block) {
                        sleeplock_acquire(&b->lk);
                        b->ref ++;
                        spinlock_release(&bio_table.lk);
                        return b;
                }
        }

        /* LRU */
        for(b = bio_table.head.prev; b->prev != &bio_table.head; b = b->prev) {
                if(b->ref == 0) {
                        sleeplock_acquire(&b->lk);
                        b->bnum = block;
                        b->ref = 1;
                        b->vaild = 0;
                         b->next = bio_table.head.next;
                        b->prev = &bio_table.head;
                        bio_table.head.next->prev = b;
                        bio_table.head.next = b;
                        spinlock_release(&bio_table.lk);
                        return b;
                }
        }
        PANIC("bio_get");
        return 0;
}