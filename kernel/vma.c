#include <linux_uapi.h>
#include <palloc.h>
#include <riscv.h>
#include <vfs.h>
#include <vma.h>

static int vma_insert_file_length(struct vma_set *set, uint64 start,
				  uint64 end, uint32 protection,
				  uint32 flags, enum vma_origin origin,
				  enum vma_usage usage,
				  struct vfs_file *file,
				  struct vma_backing *backing, uint64 offset,
				  uint64 file_length);

static struct vm_area *vma_allocate(uint64 start, uint64 end,
				    uint32 protection, uint32 flags,
				    enum vma_origin origin,
				    enum vma_usage usage,
				    struct vfs_file *file,
				    struct vma_backing *backing, uint64 offset,
				    uint64 file_length)
{
	struct vm_area *area = malloc(sizeof(*area));

	if (!area)
		return 0;
	if (file && usage == VMA_ELF && vfs_exec_mapping_get(file) < 0) {
		free(area);
		return 0;
	}
	list_init(&area->node);
	area->start = start;
	area->end = end;
	area->offset = offset;
	area->file_length = file_length;
	area->protection = protection;
	area->flags = flags;
	area->origin = origin;
	area->usage = usage;
	area->file = file ? (usage == VMA_ELF ?
		vfs_file_hold(file) : vfs_file_get(file)) : 0;
	area->backing = backing;
	if (backing)
		backing->get(backing);
	return area;
}

static struct vm_area *vma_allocate_split(const struct vm_area *area,
					  uint64 start)
{
	uint64 delta = start - area->start;
	uint64 file_length = area->file_length;
	uint64 offset = area->offset;

	if (area->origin == VMA_FILE_BACKED || area->backing) {
		offset += delta;
	}
	if (area->origin == VMA_FILE_BACKED) {
		file_length = file_length > delta ? file_length - delta : 0;
	}
	return vma_allocate(start, area->end, area->protection, area->flags,
			    area->origin, area->usage, area->file,
			    area->backing, offset,
			    file_length);
}

static void vma_release(struct vm_area *area)
{
	if (area->file) {
		if (area->usage == VMA_ELF) {
			vfs_exec_mapping_put(area->file);
			vfs_file_unhold(area->file);
		} else {
			vfs_file_put(area->file);
		}
	}
	if (area->backing)
		area->backing->put(area->backing);
	free(area);
}

static int vma_can_merge(const struct vm_area *left,
			 const struct vm_area *right)
{
	uint64 next_offset;

	if (left->end != right->start ||
	    left->protection != right->protection ||
	    left->flags != right->flags || left->origin != right->origin ||
	    left->usage != right->usage || left->file != right->file ||
	    left->backing != right->backing)
		return 0;
	if (left->origin == VMA_FILE_BACKED) {
		if (left->file_length != left->end - left->start)
			return 0;
		next_offset = left->offset + left->end - left->start;
		if (next_offset < left->offset || next_offset != right->offset)
			return 0;
	}
	if (left->backing) {
		next_offset = left->offset + left->end - left->start;
		if (next_offset < left->offset || next_offset != right->offset)
			return 0;
	}
	return 1;
}

static struct vm_area *vma_merge_pair(struct vm_area *left,
				      struct vm_area *right)
{
	if (left->origin == VMA_FILE_BACKED)
		left->file_length += right->file_length;
	left->end = right->end;
	list_remove(&right->node);
	vma_release(right);
	return left;
}

static void vma_merge_all(struct vma_set *set)
{
	struct vm_area *area, *next;
	list_t node = set->areas.next;

	while (node != &set->areas && node->next != &set->areas) {
		area = list_entry(node, struct vm_area, node);
		next = list_entry(node->next, struct vm_area, node);
		if (vma_can_merge(area, next)) {
			vma_merge_pair(area, next);
			continue;
		}
		node = node->next;
	}
}

void vma_set_init(struct vma_set *set)
{
	list_init(&set->areas);
}

void vma_set_destroy(struct vma_set *set)
{
	list_t node, next;

	for (node = set->areas.next; node != &set->areas; node = next) {
		struct vm_area *area;

		next = node->next;
		area = list_entry(node, struct vm_area, node);
		list_remove(node);
		vma_release(area);
	}
}

void vma_set_move(struct vma_set *destination, struct vma_set *source)
{
	if (destination == source)
		return;
	vma_set_destroy(destination);
	if (source->areas.next == &source->areas)
		return;
	destination->areas.next = source->areas.next;
	destination->areas.prev = source->areas.prev;
	destination->areas.next->prev = &destination->areas;
	destination->areas.prev->next = &destination->areas;
	list_init(&source->areas);
}

int vma_set_clone(struct vma_set *destination,
		  const struct vma_set *source)
{
	list_t node;

	if (destination == source)
		return 0;
	vma_set_destroy(destination);
	for (node = source->areas.next; node != &source->areas;
	     node = node->next) {
		const struct vm_area *area;

		area = list_entry(node, struct vm_area, node);
		if (vma_insert_file_length(destination, area->start, area->end,
					   area->protection, area->flags,
					   area->origin, area->usage,
					   area->file, area->backing,
					   area->offset,
					   area->file_length) < 0) {
			vma_set_destroy(destination);
			return -1;
		}
	}
	return 0;
}

static int vma_insert_file_length(struct vma_set *set, uint64 start,
				  uint64 end, uint32 protection,
				  uint32 flags, enum vma_origin origin,
				  enum vma_usage usage,
				  struct vfs_file *file,
				  struct vma_backing *backing, uint64 offset,
				  uint64 file_length)
{
	struct vm_area *area, *current;
	list_t node;

	if (!set || start >= end || start % PGSIZE || end % PGSIZE ||
	    end > MAXVA || offset % PGSIZE || file_length > end - start ||
	    ((origin == VMA_FILE_BACKED || backing) &&
	     end - start > (uint64)-1 - offset) ||
	    (origin == VMA_FILE_BACKED) != !!file ||
	    (backing && (origin != VMA_ANONYMOUS || file)) ||
	    (origin != VMA_FILE_BACKED && file_length))
		return -1;
	for (node = set->areas.next; node != &set->areas;
	     node = node->next) {
		current = list_entry(node, struct vm_area, node);
		if (end <= current->start)
			break;
		if (start < current->end)
			return -1;
	}
	area = vma_allocate(start, end, protection, flags, origin, usage,
			    file, backing, offset, file_length);
	if (!area)
		return -1;
	list_insert_before(node, &area->node);
	if (area->node.prev != &set->areas) {
		current = list_entry(area->node.prev, struct vm_area, node);
		if (vma_can_merge(current, area))
			area = vma_merge_pair(current, area);
	}
	if (area->node.next != &set->areas) {
		current = list_entry(area->node.next, struct vm_area, node);
		if (vma_can_merge(area, current))
			vma_merge_pair(area, current);
	}
	return 0;
}

int vma_insert(struct vma_set *set, uint64 start, uint64 end,
	       uint32 protection, uint32 flags, enum vma_origin origin,
	       enum vma_usage usage, struct vfs_file *file, uint64 offset)
{
	return vma_insert_file_length(set, start, end, protection, flags,
				      origin, usage, file, 0, offset,
				      origin == VMA_FILE_BACKED ? end - start : 0);
}

int vma_insert_backed(struct vma_set *set, uint64 start, uint64 end,
		      uint32 protection, uint32 flags,
		      enum vma_usage usage, struct vma_backing *backing,
		      uint64 offset)
{
	if (!backing || !backing->get || !backing->put)
		return -1;
	return vma_insert_file_length(set, start, end, protection, flags,
				      VMA_ANONYMOUS, usage, 0, backing, offset,
				      0);
}

int vma_insert_elf(struct vma_set *set, uint64 start, uint64 end,
		   uint32 protection, struct vfs_file *file, uint64 offset)
{
	return vma_insert_elf_file(set, start, end, protection, file, offset,
				    end - start);
}

int vma_insert_elf_file(struct vma_set *set, uint64 start, uint64 end,
			 uint32 protection, struct vfs_file *file,
			 uint64 offset, uint64 file_length)
{
	struct vm_area *area, *overlap;
	uint64 overlap_end;

	if (!set || !file || start >= end || start % PGSIZE ||
	    end % PGSIZE || end > MAXVA || offset % PGSIZE ||
	    end - start > (uint64)-1 - offset || file_length > end - start)
		return -1;
	overlap = (struct vm_area *)vma_find(set, start);
	if (overlap) {
		overlap_end = start + PGSIZE;
		if (overlap_end < start || overlap_end > end ||
		    overlap->end != overlap_end ||
		    overlap->usage != VMA_ELF ||
		    overlap->flags != LINUX_MAP_PRIVATE)
			return -1;
		if ((overlap->protection | protection) & LINUX_PROT_WRITE &&
		    (overlap->protection | protection) & LINUX_PROT_EXEC)
			return -1;
		if (overlap->start < start) {
			area = vma_allocate_split(overlap, start);
			if (!area)
				return -1;
			overlap->end = start;
			if (overlap->file_length > overlap->end - overlap->start)
				overlap->file_length = overlap->end -
					overlap->start;
			list_insert_after(&overlap->node, &area->node);
			overlap = area;
		}
		overlap->protection |= protection;
		if (overlap->origin == VMA_FILE_BACKED &&
		    (overlap->file != file || overlap->offset != offset)) {
			vfs_exec_mapping_put(overlap->file);
			vfs_file_unhold(overlap->file);
			overlap->file = 0;
			overlap->origin = VMA_ANONYMOUS;
			overlap->offset = 0;
			overlap->file_length = 0;
		} else if (overlap->origin == VMA_FILE_BACKED &&
			   overlap->file_length < file_length) {
			overlap->file_length = file_length < PGSIZE ?
				file_length : PGSIZE;
		}
		start = overlap_end;
		offset += PGSIZE;
		file_length = file_length > PGSIZE ?
			file_length - PGSIZE : 0;
	}
	if (start < end &&
	    vma_insert_file_length(set, start, end, protection,
				   LINUX_MAP_PRIVATE, VMA_FILE_BACKED,
				   VMA_ELF, file, 0, offset, file_length) < 0)
		return -1;
	vma_merge_all(set);
	return 0;
}

const struct vm_area *vma_find(const struct vma_set *set, uint64 address)
{
	list_t node;

	for (node = set->areas.next; node != &set->areas;
	     node = node->next) {
		const struct vm_area *area;

		area = list_entry(node, struct vm_area, node);
		if (address < area->start)
			return 0;
		if (address < area->end)
			return area;
	}
	return 0;
}

int vma_range_free(const struct vma_set *set, uint64 start, uint64 end)
{
	list_t node;

	if (start >= end)
		return 0;
	for (node = set->areas.next; node != &set->areas;
	     node = node->next) {
		const struct vm_area *area;

		area = list_entry(node, struct vm_area, node);
		if (end <= area->start)
			return 1;
		if (start < area->end)
			return 0;
	}
	return 1;
}

int vma_range_mapped(const struct vma_set *set, uint64 start, uint64 end)
{
	uint64 cursor = start;
	list_t node;

	if (start >= end)
		return 0;
	for (node = set->areas.next; node != &set->areas;
	     node = node->next) {
		const struct vm_area *area;

		area = list_entry(node, struct vm_area, node);
		if (area->end <= cursor)
			continue;
		if (area->start > cursor)
			return 0;
		if (area->end >= end)
			return 1;
		cursor = area->end;
	}
	return 0;
}

static int align_up_offset(uint64 value, uint64 alignment, uint64 offset,
			   uint64 *aligned)
{
	uint64 delta, mask = alignment - 1;
	uint64 residue = offset & mask;

	if (value <= residue) {
		*aligned = residue;
		return 0;
	}
	delta = value - residue;
	if (delta > (uint64)-1 - mask)
		return -1;
	*aligned = residue + ((delta + mask) & ~mask);
	return 0;
}

static int align_down_offset(uint64 value, uint64 alignment, uint64 offset,
			     uint64 *aligned)
{
	uint64 mask = alignment - 1;
	uint64 residue = offset & mask;

	if (value < residue)
		return -1;
	*aligned = residue + ((value - residue) & ~mask);
	return 0;
}

static int gap_aligned_address(uint64 start, uint64 end, uint64 length,
			       uint64 alignment, uint64 align_offset,
			       uint64 *address)
{
	uint64 candidate;

	if (start >= end || length > end - start ||
	    align_down_offset(end - length, alignment, align_offset,
			      &candidate) < 0 || candidate < start)
		return -1;
	*address = candidate;
	return 0;
}

int vma_find_gap_aligned(const struct vma_set *set, uint64 low, uint64 high,
			 uint64 hint, uint64 length, uint64 alignment,
			 uint64 align_offset, uint64 *address)
{
	uint64 candidate, cursor = high, gap_start;
	list_t node;

	if (!set || !address || low >= high || !length ||
	    low % PGSIZE || high % PGSIZE || length % PGSIZE ||
	    (hint && hint % PGSIZE) || alignment < PGSIZE ||
	    (alignment & (alignment - 1)) || align_offset % PGSIZE ||
	    length > high - low)
		return -1;
	if (hint >= low && hint <= high - length &&
	    align_up_offset(hint, alignment, align_offset, &candidate) == 0 &&
	    candidate <= high - length &&
	    vma_range_free(set, candidate, candidate + length)) {
		*address = candidate;
		return 0;
	}
	for (node = set->areas.prev; node != &set->areas;
	     node = node->prev) {
		const struct vm_area *area;

		area = list_entry(node, struct vm_area, node);
		if (area->start >= cursor)
			continue;
		if (area->end <= low)
			break;
		gap_start = area->end > low ? area->end : low;
		if (area->end < cursor &&
		    gap_aligned_address(gap_start, cursor, length, alignment,
					align_offset, address) == 0)
			return 0;
		if (area->start < cursor)
			cursor = area->start;
		if (cursor <= low)
			return -1;
	}
	return gap_aligned_address(low, cursor, length, alignment,
				   align_offset, address);
}

int vma_find_gap(const struct vma_set *set, uint64 low, uint64 high,
		 uint64 hint, uint64 length, uint64 *address)
{
	return vma_find_gap_aligned(set, low, high, hint, length, PGSIZE, 0,
				    address);
}

int vma_unmap(struct vma_set *set, uint64 start, uint64 end)
{
	struct vm_area *area, *split = 0;
	list_t node, next;

	if (!set || start >= end || start % PGSIZE || end % PGSIZE)
		return -1;
	area = (struct vm_area *)vma_find(set, start);
	if (area && area->start < start && area->end > end) {
		split = vma_allocate_split(area, end);
		if (!split)
			return -1;
	}
	for (node = set->areas.next; node != &set->areas; node = next) {
		uint64 old_start;

		next = node->next;
		area = list_entry(node, struct vm_area, node);
		if (area->end <= start)
			continue;
		if (area->start >= end)
			break;
		old_start = area->start;
		if (start <= area->start && end >= area->end) {
			list_remove(node);
			vma_release(area);
			continue;
		}
		if (start <= area->start) {
			uint64 delta = end - old_start;

			area->start = end;
			if (area->origin == VMA_FILE_BACKED || area->backing)
				area->offset += delta;
			if (area->origin == VMA_FILE_BACKED) {
				area->file_length = area->file_length > delta ?
					area->file_length - delta : 0;
			}
			break;
		}
		if (end >= area->end) {
			area->end = start;
			if (area->file_length > area->end - area->start)
				area->file_length = area->end - area->start;
			continue;
		}
		area->end = start;
		if (area->file_length > area->end - area->start)
			area->file_length = area->end - area->start;
		list_insert_after(&area->node, &split->node);
		split = 0;
		break;
	}
	if (split)
		vma_release(split);
	return 0;
}

int vma_protect(struct vma_set *set, uint64 start, uint64 end,
		uint32 protection)
{
	struct vm_area *area, *end_area, *start_area;
	struct vm_area *start_split = 0, *end_split = 0;
	list_t node;

	if (!set || start >= end || start % PGSIZE || end % PGSIZE ||
	    !vma_range_mapped(set, start, end))
		return -1;
	start_area = (struct vm_area *)vma_find(set, start);
	end_area = (struct vm_area *)vma_find(set, end - 1);
	if (start > start_area->start) {
		start_split = vma_allocate_split(start_area, start);
		if (!start_split)
			return -1;
	}
	if (end < end_area->end) {
		end_split = vma_allocate_split(end_area, end);
		if (!end_split) {
			if (start_split)
				vma_release(start_split);
			return -1;
		}
	}
	if (start_split) {
		start_area->end = start;
		if (start_area->file_length > start_area->end -
		    start_area->start)
			start_area->file_length = start_area->end -
						  start_area->start;
		list_insert_after(&start_area->node, &start_split->node);
	}
	end_area = (struct vm_area *)vma_find(set, end - 1);
	if (end_split) {
		end_area->end = end;
		if (end_area->file_length > end_area->end - end_area->start)
			end_area->file_length = end_area->end - end_area->start;
		list_insert_after(&end_area->node, &end_split->node);
	}
	for (node = set->areas.next; node != &set->areas;
	     node = node->next) {
		area = list_entry(node, struct vm_area, node);
		if (area->end <= start)
			continue;
		if (area->start >= end)
			break;
		area->protection = protection;
	}
	vma_merge_all(set);
	return 0;
}

int vma_count(const struct vma_set *set)
{
	int count = 0;
	list_t node;

	for (node = set->areas.next; node != &set->areas;
	     node = node->next)
		count++;
	return count;
}
