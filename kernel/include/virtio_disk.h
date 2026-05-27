#ifndef __CAFFEINIX_KERNEL_VIRTIO_DISK_H
#define __CAFFEINIX_KERNEL_VIRTIO_DISK_H

#include <riscv.h>
#include <typedefs.h>
#include <virtio.h>

/* Block size */
#define BSIZE                           1024

/* device feature bits */
#define VIRTIO_BLK_F_RO              5	/* Disk is read-only */
#define VIRTIO_BLK_F_SCSI            7	/* Supports scsi command passthru */
#define VIRTIO_BLK_F_CONFIG_WCE     11	/* Writeback mode available in config */
#define VIRTIO_BLK_F_MQ             12	/* support more than one vq */

// these are specific to virtio block devices, e.g. disks,
// described in Section 5.2 of the spec.

#define VIRTIO_BLK_T_IN  0 // read the disk
#define VIRTIO_BLK_T_OUT 1 // write the disk

// the format of the first descriptor in a disk request.
// to be followed by two more descriptors containing
// the block, and a one-byte status.
struct virtio_blk_req {
        uint32 type; // VIRTIO_BLK_T_IN or ..._OUT
        uint32 reserved;
        uint64 sector;
};

struct bio;

void virtio_disk_rw(struct bio *b, int write);
void virtio_disk_init(void);
void virtio_disk_intr(void);

#endif
