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

static struct {
	struct sleeplock lock;
	struct list processes;
} mmap_registry;

void mmap_init(void)
{
	sleeplock_init(&mmap_registry.lock, "mmap registry");
	list_init(&mmap_registry.processes);
}

void mmap_process_register(process_t process)
{
	if (!process)
		PANIC("register null mmap");
	sleeplock_acquire(&mmap_registry.lock);
	if (process->mmap_registered)
		PANIC("register mmap twice");
	list_insert_after(&mmap_registry.processes, &process->mmap_tag);
	process->mmap_registered = 1;
	sleeplock_release(&mmap_registry.lock);
}

void mmap_process_unregister(process_t process)
{
	if (!process)
		PANIC("unregister null mmap");
	sleeplock_acquire(&mmap_registry.lock);
	if (!process->mmap_registered)
		PANIC("unregister inactive mmap");
	list_remove(&process->mmap_tag);
	process->mmap_registered = 0;
	sleeplock_release(&mmap_registry.lock);
}

int mmap_process_fork(process_t parent, process_t child)
{
	int result = -1;

	if (!parent || !child)
		return -1;
	sleeplock_acquire(&mmap_registry.lock);
	sleeplock_acquire(&parent->mmap_lock);
	if (vm_copy(parent->pagetable, child->pagetable, &parent->vmas) < 0 ||
	    vma_set_clone(&child->vmas, &parent->vmas) < 0)
		goto out;
	child->sz = parent->sz;
	child->brk = parent->brk;
	child->brk_start = parent->brk_start;
	list_insert_after(&mmap_registry.processes, &child->mmap_tag);
	child->mmap_registered = 1;
	result = 0;
out:
	sleeplock_release(&parent->mmap_lock);
	sleeplock_release(&mmap_registry.lock);
	return result;
}

static int mmap_area_matches_inode(const struct vm_area *area,
				   const struct vfs_inode *inode)
{
	struct vfs_inode *mapped;

	if (!area || area->origin != VMA_FILE_BACKED || !area->file ||
	    !area->file->path.dentry ||
	    !(mapped = area->file->path.dentry->inode))
		return 0;
	return mapped->superblock == inode->superblock &&
	       mapped->number == inode->number;
}

static void mmap_truncate_process(process_t process,
				  struct vfs_inode *inode, uint64 size)
{
	struct vm_area *area;
	uint64 delta, start;
	list_t node;

	sleeplock_acquire(&process->mmap_lock);
	for (node = process->vmas.areas.next;
	     node != &process->vmas.areas; node = node->next) {
		area = list_entry(node, struct vm_area, node);
		if (!mmap_area_matches_inode(area, inode))
			continue;
		if (size <= area->offset)
			start = area->start;
		else {
			delta = size - area->offset;
			if (delta >= area->end - area->start)
				continue;
			start = area->start + PGROUNDDOWN(delta);
		}
		vm_unmap_range(process->pagetable, start, area->end - start);
	}
	sleeplock_release(&process->mmap_lock);
}

int mmap_file_truncate(struct vfs_inode *inode, uint64 old_size,
		       uint64 size)
{
	process_t process;
	list_t node;
	int result;

	if (!inode)
		return -1;
	do {
		sleeplock_acquire(&mmap_registry.lock);
		if (size < old_size) {
			for (node = mmap_registry.processes.next;
			     node != &mmap_registry.processes;
			     node = node->next) {
				process = list_entry(node, struct process, mmap_tag);
				mmap_truncate_process(process, inode, size);
			}
		}
		result = page_cache_truncate(inode, old_size, size);
		sleeplock_release(&mmap_registry.lock);
		if (result == PAGE_CACHE_TRUNCATE_RETRY)
			yield();
	} while (result == PAGE_CACHE_TRUNCATE_RETRY);
	return result;
}

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
			      uint64 length, int protection, int flags,
			      struct vfs_stat *stat)
{
	if (!file || !(file->flags & VFS_OPEN_READ))
		return LINUX_EACCES;
	if ((flags & 0xf) == LINUX_MAP_SHARED &&
	    (protection & LINUX_PROT_WRITE) &&
	    !(file->flags & VFS_OPEN_WRITE))
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
					      void **page,
					      int *cached)
{
	struct vfs_stat stat;
	uint64 bytes, file_offset, relative;
	void *allocated;
	enum page_cache_get_result cache_result;
	int64 result;

	*cached = 0;

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
	if (area->usage == VMA_MMAP || bytes == PGSIZE) {
		cache_result = page_cache_get(area->file, file_offset, bytes,
					      page);
		if (cache_result == PAGE_CACHE_GET_OK) {
			*cached = 1;
			return MMAP_FAULT_OK;
		}
		if (cache_result == PAGE_CACHE_GET_IO)
			return area->usage == VMA_ELF ? MMAP_FAULT_NOMEM :
				MMAP_FAULT_BUSERR;
	}
	if ((area->flags & 0xf) == LINUX_MAP_SHARED)
		return MMAP_FAULT_NOMEM;
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
	int cached = 0;
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
		int cow = access == MMAP_FAULT_WRITE ?
			vm_resolve_cow(process->pagetable, page_address) : 0;

		if (cow > 0) {
			result = MMAP_FAULT_OK;
			goto out;
		}
		if (cow < 0) {
			result = MMAP_FAULT_NOMEM;
			goto out;
		}
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
		result = mmap_file_fault(area, page_address, &page, &cached);
	else {
		page = palloc_zero();
		if (!page && page_cache_reclaim(1))
			page = palloc_zero();
		result = page ? MMAP_FAULT_OK : MMAP_FAULT_NOMEM;
	}
	if (result != MMAP_FAULT_OK)
		goto out;
	permissions = mmap_pte_permissions(area->protection);
	if (cached && (area->flags & 0xf) == LINUX_MAP_PRIVATE &&
	    (permissions & PTE_W)) {
		permissions &= ~PTE_W;
		permissions |= PTE_SW_COW;
	}
	if (cached && (area->flags & 0xf) == LINUX_MAP_SHARED &&
	    (permissions & PTE_W) &&
	    page_cache_mark_dirty(area->file,
				  area->offset + page_address - area->start) < 0) {
		pfree(page);
		result = MMAP_FAULT_NOMEM;
		goto out;
	}
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
	    ((flags & 0xf) != LINUX_MAP_PRIVATE &&
	     (flags & 0xf) != LINUX_MAP_SHARED) ||
	    flags & ~(LINUX_MAP_SHARED | LINUX_MAP_PRIVATE | LINUX_MAP_FIXED |
		      LINUX_MAP_ANONYMOUS))
		return -LINUX_EINVAL;
	if ((flags & LINUX_MAP_ANONYMOUS) &&
	    (flags & 0xf) == LINUX_MAP_SHARED)
		return -LINUX_EINVAL;
	if ((flags & LINUX_MAP_FIXED) && address % PGSIZE)
		return -LINUX_EINVAL;
	if (flags & LINUX_MAP_ANONYMOUS) {
		origin = VMA_ANONYMOUS;
	} else {
		origin = VMA_FILE_BACKED;
		if (vfs_get_file_fd(fd, &file) < 0)
			return -LINUX_EBADF;
		error = mmap_file_validate(file, offset, length, protection,
				   flags, &stat);
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

static int mmap_mark_shared_dirty(process_t process, uint64 start,
				  uint64 end)
{
	const struct vm_area *area;
	uint64 address, offset;

	for (address = start; address < end; address += PGSIZE) {
		if (!vm_mapped(process->pagetable, address))
			continue;
		area = vma_find(&process->vmas, address);
		if (!area || area->origin != VMA_FILE_BACKED ||
		    (area->flags & 0xf) != LINUX_MAP_SHARED ||
		    !(area->protection & LINUX_PROT_WRITE))
			continue;
		offset = area->offset + address - area->start;
		if (page_cache_mark_dirty(area->file, offset) < 0)
			return -1;
	}
	return 0;
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
	if ((protection & LINUX_PROT_WRITE) &&
	    mmap_mark_shared_dirty(process, address, end) < 0)
		PANIC("shared mapping cache mismatch");
	if (vm_protect_user_range(process->pagetable, address, end,
				  permissions, &process->vmas) < 0)
		PANIC("VMA and page table protection mismatch");
	sleeplock_release(&process->mmap_lock);
	return 0;
}

uint64 sys_linux_msync(void)
{
	process_t process = cur_proc();
	const struct vm_area *area;
	uint64 address, end, length;
	int flags, result = 0;
	list_t node;

	argaddr(0, &address);
	argaddr(1, &length);
	argint(2, &flags);
	if (address % PGSIZE || mmap_round_length(length, &length) < 0 ||
	    address > USER_STACK_TOP || length > USER_STACK_TOP - address ||
	    flags & ~(LINUX_MS_ASYNC | LINUX_MS_INVALIDATE |
		      LINUX_MS_SYNC) ||
	    (flags & LINUX_MS_ASYNC && flags & LINUX_MS_SYNC))
		return -LINUX_EINVAL;
	end = address + length;
	sleeplock_acquire(&process->mmap_lock);
	if (!vma_range_mapped(&process->vmas, address, end)) {
		result = -LINUX_ENOMEM;
		goto out;
	}
	for (node = process->vmas.areas.next;
	     node != &process->vmas.areas; node = node->next) {
		area = list_entry(node, struct vm_area, node);
		if (area->end <= address)
			continue;
		if (area->start >= end)
			break;
		if (area->origin == VMA_FILE_BACKED &&
		    (area->flags & 0xf) == LINUX_MAP_SHARED &&
		    page_cache_writeback_file(area->file) < 0) {
			result = -LINUX_EIO;
			break;
		}
	}
out:
	sleeplock_release(&process->mmap_lock);
	return result;
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
