#include <virtio_disk.h>
#include <spinlock.h>
#include <debug.h>
#include <string.h>
#include <palloc.h>
#include <mem_layout.h>
#include <process.h>

#define BSIZE                   1024

#define NUM                     VIRTIO_DES_NUM

#define initlock(x, name)       spinlock_init(x, name)
#define acquire(x)              spinlock_acquire(x)
#define release(x)              spinlock_release(x)
#define panic(x)                PANIC(x)
#define kalloc                  palloc

// the address of virtio mmio register r.
#define R(r) ((volatile uint32 *)(VIRTIO0 + (r)))

static struct disk {
        // a set (not a ring) of DMA descriptors, with which the
        // driver tells the device where to read and write individual
        // disk operations. there are NUM descriptors.
        // most commands consist of a "chain" (a linked list) of a couple of
        // these descriptors.
        struct virtq_desc *desc;

        // a ring in which the driver writes descriptor numbers
        // that the driver would like the device to process.  it only
        // includes the head descriptor of each chain. the ring has
        // NUM elements.
        struct virtq_avail *avail;

        // a ring in which the device writes descriptor numbers that
        // the device has finished processing (just the head of each chain).
        // there are NUM used ring entries.
        struct virtq_used *used;

        // our own book-keeping.
        char free[VIRTIO_DES_NUM];  // is a descriptor free?
        uint16 used_idx; // we've looked this far in used[2..NUM].

        // track info about in-flight operations,
        // for use when completion interrupt arrives.
        // indexed by first descriptor index of chain.
        struct {
        struct buf *b;
        char status;
        } info[VIRTIO_DES_NUM];

        // disk command headers.
        // one-for-one with descriptors, for convenience.
        struct virtio_blk_req ops[VIRTIO_DES_NUM];

        struct spinlock vdisk_lock;
  
} disk;

void virtio_disk_init(void)
{
        uint32 status = 0;

        initlock(&disk.vdisk_lock, "virtio_disk");

        if(*R(VIRTIO_MMIO_MAGIC_VALUE) != 0x74726976 ||
        *R(VIRTIO_MMIO_VERSION) != 2 ||
        *R(VIRTIO_MMIO_DEVICE_ID) != 2 ||
        *R(VIRTIO_MMIO_VENDOR_ID) != 0x554d4551){
        panic("could not find virtio disk");
        }

        // reset device
        *R(VIRTIO_MMIO_STATUS) = status;

        // set ACKNOWLEDGE status bit
        status |= VIRTIO_CONFIG_S_ACKNOWLEDGE;
        *R(VIRTIO_MMIO_STATUS) = status;

        // set DRIVER status bit
        status |= VIRTIO_CONFIG_S_DRIVER;
        *R(VIRTIO_MMIO_STATUS) = status;

        // negotiate features
        uint64 features = *R(VIRTIO_MMIO_DEVICE_FEATURES);
        features &= ~(1 << VIRTIO_BLK_F_RO);
        features &= ~(1 << VIRTIO_BLK_F_SCSI);
        features &= ~(1 << VIRTIO_BLK_F_CONFIG_WCE);
        features &= ~(1 << VIRTIO_BLK_F_MQ);
        features &= ~(1 << VIRTIO_F_ANY_LAYOUT);
        features &= ~(1 << VIRTIO_RING_F_EVENT_IDX);
        features &= ~(1 << VIRTIO_RING_F_INDIRECT_DESC);
        *R(VIRTIO_MMIO_DRIVER_FEATURES) = features;

        // tell device that feature negotiation is complete.
        status |= VIRTIO_CONFIG_S_FEATURES_OK;
        *R(VIRTIO_MMIO_STATUS) = status;

        // re-read status to ensure FEATURES_OK is set.
        status = *R(VIRTIO_MMIO_STATUS);
        if(!(status & VIRTIO_CONFIG_S_FEATURES_OK))
        panic("virtio disk FEATURES_OK unset");

        // initialize queue 0.
        *R(VIRTIO_MMIO_QUEUE_SEL) = 0;

        // ensure queue 0 is not in use.
        if(*R(VIRTIO_MMIO_QUEUE_READY))
        panic("virtio disk should not be ready");

        // check maximum queue size.
        uint32 max = *R(VIRTIO_MMIO_QUEUE_NUM_MAX);
        if(max == 0)
        panic("virtio disk has no queue 0");
        if(max < NUM)
        panic("virtio disk max queue too short");

        // allocate and zero queue memory.
        disk.desc = kalloc();
        disk.avail = kalloc();
        disk.used = kalloc();
        if(!disk.desc || !disk.avail || !disk.used)
        panic("virtio disk kalloc");
        memset(disk.desc, 0, PGSIZE);
        memset(disk.avail, 0, PGSIZE);
        memset(disk.used, 0, PGSIZE);

        // set queue size.
        *R(VIRTIO_MMIO_QUEUE_NUM) = NUM;

        // write physical addresses.
        *R(VIRTIO_MMIO_QUEUE_DESC_LOW) = (uint64)disk.desc;
        *R(VIRTIO_MMIO_QUEUE_DESC_HIGH) = (uint64)disk.desc >> 32;
        *R(VIRTIO_MMIO_DRIVER_DESC_LOW) = (uint64)disk.avail;
        *R(VIRTIO_MMIO_DRIVER_DESC_HIGH) = (uint64)disk.avail >> 32;
        *R(VIRTIO_MMIO_DEVICE_DESC_LOW) = (uint64)disk.used;
        *R(VIRTIO_MMIO_DEVICE_DESC_HIGH) = (uint64)disk.used >> 32;

        // queue is ready.
        *R(VIRTIO_MMIO_QUEUE_READY) = 0x1;

        // all NUM descriptors start out unused.
        for(int i = 0; i < NUM; i++)
        disk.free[i] = 1;

        // tell device we're completely ready.
        status |= VIRTIO_CONFIG_S_DRIVER_OK;
        *R(VIRTIO_MMIO_STATUS) = status;
}

// find a free descriptor, mark it non-free, return its index.
static int alloc_desc(void)
{
        for(int i = 0; i < NUM; i++){
        if(disk.free[i]){
        disk.free[i] = 0;
        return i;
        }
        }
        return -1;
}

// mark a descriptor as free.
static void free_desc(int i)
{
        if(i >= NUM)
        panic("free_desc 1");
        if(disk.free[i])
        panic("free_desc 2");
        disk.desc[i].addr = 0;
        disk.desc[i].len = 0;
        disk.desc[i].flags = 0;
        disk.desc[i].next = 0;
        disk.free[i] = 1;
        wakeup(&disk.free[0]);
}

// free a chain of descriptors.
static void free_chain(int i)
{
        while(1){
        int flag = disk.desc[i].flags;
        int nxt = disk.desc[i].next;
        free_desc(i);
        if(flag & VRING_DESC_F_NEXT)
        i = nxt;
        else
        break;
        }
}

// allocate three descriptors (they need not be contiguous).
// disk transfers always use three descriptors.
static int alloc3_desc(int *idx)
{
        for(int i = 0; i < 3; i++){
        idx[i] = alloc_desc();
        if(idx[i] < 0){
        for(int j = 0; j < i; j++)
        free_desc(idx[j]);
        return -1;
        }
        }
        return 0;
}

void virtio_disk_rw(void* buf, uint16 block,  int write)
{
        // uint64 sector = b->blockno * (BSIZE / 512);
        uint64 sector = block * (BSIZE / 512);

        acquire(&disk.vdisk_lock);

        // the spec's Section 5.2 says that legacy block operations use
        // three descriptors: one for type/reserved/sector, one for the
        // data, one for a 1-byte status result.

        // allocate the three descriptors.
        int idx[3];
        while(1){
        if(alloc3_desc(idx) == 0) {
        break;
        }
        sleep(&disk.free[0], &disk.vdisk_lock);
        }

        // format the three descriptors.
        // qemu's virtio-blk.c reads them.

        struct virtio_blk_req *buf0 = &disk.ops[idx[0]];

        if(write)
        buf0->type = VIRTIO_BLK_T_OUT; // write the disk
        else
        buf0->type = VIRTIO_BLK_T_IN; // read the disk
        buf0->reserved = 0;
        buf0->sector = sector;

        disk.desc[idx[0]].addr = (uint64) buf0;
        disk.desc[idx[0]].len = sizeof(struct virtio_blk_req);
        disk.desc[idx[0]].flags = VRING_DESC_F_NEXT;
        disk.desc[idx[0]].next = idx[1];

        // disk.desc[idx[1]].addr = (uint64) b->data;
        disk.desc[idx[1]].addr = (uint64) buf;
        disk.desc[idx[1]].len = BSIZE;
        if(write)
        disk.desc[idx[1]].flags = 0; // device reads b->data
        else
        disk.desc[idx[1]].flags = VRING_DESC_F_WRITE; // device writes b->data
        disk.desc[idx[1]].flags |= VRING_DESC_F_NEXT;
        disk.desc[idx[1]].next = idx[2];

        disk.info[idx[0]].status = 0xff; // device writes 0 on success
        disk.desc[idx[2]].addr = (uint64) &disk.info[idx[0]].status;
        disk.desc[idx[2]].len = 1;
        disk.desc[idx[2]].flags = VRING_DESC_F_WRITE; // device writes the status
        disk.desc[idx[2]].next = 0;

        // record struct buf for virtio_disk_intr().
        // b->disk = 1;
        // disk.info[idx[0]].b = b;

        // tell the device the first index in our chain of descriptors.
        disk.avail->ring[disk.avail->idx % NUM] = idx[0];

        __sync_synchronize();

        // tell the device another avail ring entry is available.
        disk.avail->idx += 1; // not % NUM ...

        __sync_synchronize();

        *R(VIRTIO_MMIO_QUEUE_NOTIFY) = 0; // value is queue number

        // Wait for virtio_disk_intr() to say request has finished.
        // while(b->disk == 1) {
        //         sleep(b, &disk.vdisk_lock);
        // }

        disk.info[idx[0]].b = 0;
        free_chain(idx[0]);

        release(&disk.vdisk_lock);
}