#include <block_device.h>
#include <ff.h>
#include <diskio.h>
#include <fatfs.h>

static struct block_device *fatfs_block_device;

void fatfs_set_block_device(struct block_device *device)
{
	fatfs_block_device = device;
}

DSTATUS disk_initialize(BYTE drive)
{
	return drive || !fatfs_block_device ? STA_NOINIT : 0;
}

DSTATUS disk_status(BYTE drive)
{
	return disk_initialize(drive);
}

DRESULT disk_read(BYTE drive, BYTE *buffer, LBA_t sector, UINT count)
{
	if (drive || !fatfs_block_device || !count)
		return RES_PARERR;
	return block_device_read(fatfs_block_device, sector, buffer, count) ?
		RES_ERROR : RES_OK;
}

DRESULT disk_write(BYTE drive, const BYTE *buffer, LBA_t sector,
		   UINT count)
{
	if (drive || !fatfs_block_device || !count)
		return RES_PARERR;
	return block_device_write(fatfs_block_device, sector, buffer, count) ?
		RES_ERROR : RES_OK;
}

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
