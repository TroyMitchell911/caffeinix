#include <debug.h>
#include <linux_uapi.h>
#include <mem_layout.h>
#include <mmap.h>
#include <mystring.h>
#include <page_cache.h>
#include <palloc.h>
#include <process.h>
#include <scheduler.h>
#include <syscall.h>
#include <vfs.h>
#include <vm.h>
#include <vma.h>

static int mmap_protection_valid(int protection)
{
	return !(protection & ~(LINUX_PROT_READ | LINUX_PROT_WRITE |
			       LINUX_PROT_EXEC));
}

static int mmap_pte_permissions(int protection)
{
	int permissions = 0;

	if (protection == LINUX_PROT_NONE)
		return PTE_R;
	permissions |= PTE_U;
	if (protection & (LINUX_PROT_READ | LINUX_PROT_WRITE))
		permissions |= PTE_R;
	if (protection & LINUX_PROT_WRITE)
		permissions |= PTE_W;
	if (protection & LINUX_PROT_EXEC)
		permissions |= PTE_X;
	return permissions;
}

static int mmap_round_length(uint64 length, uint64 *rounded)
{
	if (!length || length > (uint64)-1 - (PGSIZE - 1))
		return -1;
	*rounded = PGROUNDUP(length);
	return *rounded ? 0 : -1;
}

static int mmap_file_validate(struct vfs_file *file, uint64 offset,
			      uint64 length, struct vfs_stat *stat)
{
	if (!file || !(file->flags & VFS_OPEN_READ))
		return LINUX_EACCES;
	if (offset % PGSIZE)
		return LINUX_EINVAL;
	if (length > (uint64)-1 - offset)
		return LINUX_EOVERFLOW;
	if (!file->path.dentry || !file->path.dentry->inode ||
	    vfs_inode_stat(file->path.dentry->inode, stat) < 0)
		return LINUX_EIO;
	if (stat->type != VFS_INODE_REGULAR)
		return LINUX_ENODEV;
	return 0;
}

static enum mmap_fault_result mmap_file_fault(struct vm_area *area,
					      uint64 page_address,
					      void **page)
{
	struct vfs_stat stat;
	uint64 bytes, file_offset, relative;
	void *allocated;
	int64 result;

	relative = page_address - area->start;
	file_offset = area->offset + relative;
	if (area->usage == VMA_ELF) {
		if (relative >= area->file_length) {
			*page = palloc_zero();
			return *page ? MMAP_FAULT_OK : MMAP_FAULT_NOMEM;
		}
		bytes = area->file_length - relative;
		if (bytes > PGSIZE)
			bytes = PGSIZE;
	} else {
		if (vfs_inode_stat(area->file->path.dentry->inode, &stat) < 0)
			return MMAP_FAULT_BUSERR;
		if (file_offset >= stat.size)
			return MMAP_FAULT_BUSERR;
		bytes = stat.size - file_offset;
		if (bytes > PGSIZE)
			bytes = PGSIZE;
	}
	/*
	 * A private mmap may later become writable through mprotect().  Until
	 * private cache mappings have copy-on-write PTEs, only share immutable
	 * load mappings selected by the ELF loader.
	 */
	if (bytes == PGSIZE && area->usage == VMA_ELF &&
	    !(area->protection & LINUX_PROT_WRITE) &&
	    page_cache_get(area->file, file_offset, page) == 0)
		return MMAP_FAULT_OK;
	allocated = palloc_zero();
	if (!allocated && page_cache_reclaim(1))
		allocated = palloc_zero();
	if (!allocated)
		return MMAP_FAULT_NOMEM;
	result = vfs_file_pread(area->file, 0, (uint64)allocated, bytes,
				file_offset);
	if (result != (int64)bytes) {
		pfree(allocated);
		return area->usage == VMA_ELF ? MMAP_FAULT_NOMEM :
			MMAP_FAULT_BUSERR;
	}
	*page = allocated;
	return MMAP_FAULT_OK;
}

static int mmap_access_allowed(const struct vm_area *area,
			       enum mmap_fault_access access)
{
	if (access == MMAP_FAULT_EXEC)
		return area->protection & LINUX_PROT_EXEC;
	if (access == MMAP_FAULT_WRITE)
		return area->protection & LINUX_PROT_WRITE;
	return area->protection & LINUX_PROT_READ;
}

enum mmap_fault_result mmap_handle_fault(process_t process, uint64 address,
					 enum mmap_fault_access access)
{
	struct vm_area *area;
	enum mmap_fault_result result = MMAP_FAULT_NOMEM;
	uint64 page_address = PGROUNDDOWN(address);
	int permissions;
	void *page = 0;

	if (!process || address >= MAXVA)
		return MMAP_FAULT_MAPERR;
	sleeplock_acquire(&process->mmap_lock);
	area = (struct vm_area *)vma_find(&process->vmas, address);
	if (!area) {
		result = MMAP_FAULT_MAPERR;
		goto out;
	}
	if (!mmap_access_allowed(area, access)) {
		result = MMAP_FAULT_ACCERR;
		goto out;
	}
	if (vm_mapped(process->pagetable, page_address)) {
		pte_t *pte = PTE(process->pagetable, page_address, 0);

		if (pte && (*pte & PTE_V) && (*pte & PTE_U) &&
		    ((access == MMAP_FAULT_READ && (*pte & PTE_R)) ||
		     (access == MMAP_FAULT_WRITE && (*pte & PTE_W)) ||
		     (access == MMAP_FAULT_EXEC && (*pte & PTE_X)))) {
			result = MMAP_FAULT_OK;
			goto out;
		}
		result = MMAP_FAULT_ACCERR;
		goto out;
	}
	if (area->origin == VMA_FILE_BACKED)
		result = mmap_file_fault(area, page_address, &page);
	else {
		page = palloc_zero();
		if (!page && page_cache_reclaim(1))
			page = palloc_zero();
		result = page ? MMAP_FAULT_OK : MMAP_FAULT_NOMEM;
	}
	if (result != MMAP_FAULT_OK)
		goto out;
	permissions = mmap_pte_permissions(area->protection);
	if (vm_map(process->pagetable, page_address, (uint64)page, PGSIZE,
		   permissions | PTE_SW_USER) < 0) {
		pfree(page);
		result = MMAP_FAULT_NOMEM;
		goto out;
	}
	sfence_vma();
	if (area->protection & LINUX_PROT_EXEC)
		fence_i();
out:
	sleeplock_release(&process->mmap_lock);
	return result;
}

uint64 sys_linux_brk(void)
{
	process_t process = cur_proc();
	uint64 new_end, old_end, requested, result;

	argaddr(0, &requested);
	if (!requested)
		return process->brk;
	sleeplock_acquire(&process->mmap_lock);
	result = process->brk;
	if (requested < process->brk_start || requested > USER_MMAP_TOP ||
	    requested > (uint64)-1 - (PGSIZE - 1))
		goto out;
	old_end = PGROUNDUP(process->brk);
	new_end = PGROUNDUP(requested);
	if (new_end > old_end) {
		if (!vma_range_free(&process->vmas, old_end, new_end) ||
		    vma_insert(&process->vmas, old_end, new_end,
			       LINUX_PROT_READ | LINUX_PROT_WRITE,
			       LINUX_MAP_PRIVATE,
			       VMA_ANONYMOUS, VMA_HEAP, 0, 0) < 0) {
			goto out;
		}
	} else if (new_end < old_end) {
		if (vma_unmap(&process->vmas, new_end, old_end) < 0)
			goto out;
		vm_unmap_range(process->pagetable, new_end, old_end - new_end);
	}
	process->brk = requested;
	if (process->sz < requested)
		process->sz = requested;
	result = requested;

out:
	sleeplock_release(&process->mmap_lock);
	return result;
}

uint64 sys_linux_mmap(void)
{
	process_t process = cur_proc();
	struct vfs_file *file = 0;
	struct vfs_stat stat;
	uint64 address, end, hint, length, offset, start = 0;
	uint64 result = -LINUX_ENOMEM;
	int error, fd, flags, protection;
	enum vma_origin origin;

	argaddr(0, &address);
	argaddr(1, &length);
	argint(2, &protection);
	argint(3, &flags);
	argint(4, &fd);
	argaddr(5, &offset);
	if (!mmap_protection_valid(protection) ||
	    mmap_round_length(length, &length) < 0 || offset % PGSIZE ||
	    (flags & 0xf) != LINUX_MAP_PRIVATE ||
	    flags & ~(LINUX_MAP_PRIVATE | LINUX_MAP_FIXED |
		      LINUX_MAP_ANONYMOUS))
		return -LINUX_EINVAL;
	if ((flags & LINUX_MAP_FIXED) && address % PGSIZE)
		return -LINUX_EINVAL;
	if (flags & LINUX_MAP_ANONYMOUS) {
		origin = VMA_ANONYMOUS;
	} else {
		origin = VMA_FILE_BACKED;
		if (vfs_get_file_fd(fd, &file) < 0)
			return -LINUX_EBADF;
		error = mmap_file_validate(file, offset, length, &stat);
		if (error) {
			result = -error;
			goto out_file;
		}
	}

	sleeplock_acquire(&process->mmap_lock);
	if (flags & LINUX_MAP_FIXED) {
		start = address;
		if (start < PGSIZE || start % PGSIZE ||
		    start > USER_MMAP_TOP || length > USER_MMAP_TOP - start) {
			result = -LINUX_ENOMEM;
			goto out_unlock;
		}
		end = start + length;
		if (vma_unmap(&process->vmas, start, end) < 0) {
			result = -LINUX_ENOMEM;
			goto out_unlock;
		}
		vm_unmap_range(process->pagetable, start, length);
	} else {
		uint64 low = PGROUNDUP(process->brk);

		if (low < PGSIZE)
			low = PGSIZE;
		hint = address ? PGROUNDDOWN(address) : 0;
		if (hint < low || hint > USER_MMAP_TOP - length)
			hint = 0;
		if (vma_find_gap(&process->vmas, low, USER_MMAP_TOP,
				 hint, length, &start) < 0)
			goto out_unlock;
		end = start + length;
	}
	if (vma_insert(&process->vmas, start, end, protection, flags & 0xf,
		       origin, VMA_MMAP, file, offset) < 0)
		goto out_unlock;
	result = start;
	goto out_unlock;
out_unlock:
	sleeplock_release(&process->mmap_lock);
out_file:
	if (file)
		vfs_file_put(file);
	return result;
}

uint64 sys_linux_mprotect(void)
{
	process_t process = cur_proc();
	uint64 address, end, length;
	int permissions, protection;

	argaddr(0, &address);
	argaddr(1, &length);
	argint(2, &protection);
	if (!mmap_protection_valid(protection) || address % PGSIZE)
		return -LINUX_EINVAL;
	if (!length)
		return 0;
	if (mmap_round_length(length, &length) < 0 ||
	    address > USER_STACK_TOP || length > USER_STACK_TOP - address)
		return -LINUX_ENOMEM;
	end = address + length;
	permissions = mmap_pte_permissions(protection);
	sleeplock_acquire(&process->mmap_lock);
	if (!vma_range_mapped(&process->vmas, address, end)) {
		sleeplock_release(&process->mmap_lock);
		return -LINUX_ENOMEM;
	}
	if (vma_protect(&process->vmas, address, end, protection) < 0) {
		sleeplock_release(&process->mmap_lock);
		return -LINUX_ENOMEM;
	}
	if (vm_protect_user_range(process->pagetable, address, end,
				  permissions) < 0)
		PANIC("VMA and page table protection mismatch");
	sleeplock_release(&process->mmap_lock);
	return 0;
}

uint64 sys_linux_munmap(void)
{
	process_t process = cur_proc();
	uint64 address, end, length;

	argaddr(0, &address);
	argaddr(1, &length);
	if (address % PGSIZE || mmap_round_length(length, &length) < 0 ||
	    address > USER_STACK_TOP || length > USER_STACK_TOP - address)
		return -LINUX_EINVAL;
	end = address + length;
	sleeplock_acquire(&process->mmap_lock);
	if (vma_unmap(&process->vmas, address, end) < 0) {
		sleeplock_release(&process->mmap_lock);
		return -LINUX_ENOMEM;
	}
	vm_unmap_range(process->pagetable, address, length);
	sleeplock_release(&process->mmap_lock);
	return 0;
}
