/*
 * FatFs disk I/O glue.
 *
 * FatFs calls this narrow adapter for its sole configured volume.  The VFS
 * FAT backend selects the backing block device before mounting; callers must
 * not change it while FatFs has live files.
 */
#include <block_device.h>
#include <ff.h>
#include <diskio.h>
#include <fatfs.h>

/* FatFs has one configured volume, selected before its mount operation. */
static struct block_device *fatfs_block_device;

/* Select the sole drive before any FatFs mount or I/O can use it. */
void fatfs_set_block_device(struct block_device *device)
{
	fatfs_block_device = device;
}

/**
 * disk_initialize() - Serve the FatFs block-device callback
 * @drive: FatFs volume index; only drive zero is configured.
 *
 * Context: Any context; does not perform I/O or sleep.
 *
 * Return: Zero when drive zero has a backing device, otherwise STA_NOINIT.
 */
DSTATUS disk_initialize(BYTE drive)
{
	return drive || !fatfs_block_device ? STA_NOINIT : 0;
}

/**
 * disk_status() - Serve the FatFs block-device callback
 * @drive: FatFs volume index; only drive zero is configured.
 *
 * Context: Any context; does not perform I/O or sleep.
 *
 * Return: The same FatFs status bits as disk_initialize().
 */
DSTATUS disk_status(BYTE drive)
{
	return disk_initialize(drive);
}

/**
 * disk_read() - Serve the FatFs block-device callback
 * @drive: FatFs volume index; only drive zero is configured.
 * @buffer: FatFs-owned sector buffer valid for the callback duration.
 * @sector: First 512-byte logical sector to transfer.
 * @count: Nonzero number of sectors to transfer.
 *
 * Context: Current process context; may sleep in VFS, a filesystem,
 * or device.
 *
 * Return: RES_OK, RES_PARERR for invalid arguments, or RES_ERROR for a
 * backing-device read failure.
 */
DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count)
{
	if (drive || !fatfs_block_device || !count)
		return RES_PARERR;
	return block_device_read(fatfs_block_device, sector, buffer, count) ?
		RES_ERROR : RES_OK;
}

/**
 * disk_write() - Serve the FatFs block-device callback
 * @drive: FatFs volume index; only drive zero is configured.
 * @buffer: FatFs-owned sector buffer valid for the callback duration.
 * @sector: First 512-byte logical sector to transfer.
 * @count: Nonzero number of sectors to transfer.
 *
 * Context: Current process context; may sleep in VFS, a filesystem,
 * or device.
 *
 * Return: RES_OK, RES_PARERR for invalid arguments, or RES_ERROR for a
 * backing-device write failure.
 */
DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector,
		   UINT count)
{
	if (drive || !fatfs_block_device || !count)
		return RES_PARERR;
	return block_device_write(fatfs_block_device, sector, buffer, count) ?
		RES_ERROR : RES_OK;
}

/**
 * disk_ioctl() - Serve the FatFs block-device callback
 * @drive: FatFs volume index; only drive zero is configured.
 * @command: FatFs disk-control command.
 * @buffer: Output storage required by geometry commands; unused by CTRL_SYNC.
 *
 * Context: Current process context; may sleep in VFS, a filesystem,
 * or device.
 * Return: RES_OK, RES_PARERR for an unsupported command or invalid argument,
 * or RES_ERROR when CTRL_SYNC flush fails.
 */
DRESULT disk_ioctl(BYTE drive, BYTE command, void *buffer)
{
	if (drive || !fatfs_block_device)
		return RES_PARERR;
	switch (command) {
	case CTRL_SYNC:
		return block_device_flush(fatfs_block_device) ?
			RES_ERROR : RES_OK;
	case GET_SECTOR_COUNT:
		if (!buffer || fatfs_block_device->sector_count > 0xffffffffULL)
			return RES_PARERR;
		*(DWORD *)buffer = fatfs_block_device->sector_count;
		return RES_OK;
	case GET_SECTOR_SIZE:
		if (!buffer)
			return RES_PARERR;
		*(WORD *)buffer = fatfs_block_device->sector_size;
		return RES_OK;
	case GET_BLOCK_SIZE:
		if (!buffer)
			return RES_PARERR;
		*(DWORD *)buffer = 1;
		return RES_OK;
	default:
		return RES_PARERR;
	}
}
