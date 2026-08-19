#include <stdio.h>
#include <stdlib.h>

#include <linux_uapi.h>
#include <vfs.h>
#include <vma.h>

#define CHECK(condition) do { \
	if (!(condition)) { \
		fprintf(stderr, "VMA check failed at line %d\n", __LINE__); \
		exit(EXIT_FAILURE); \
	} \
} while (0)

static struct vfs_file files[2];
static int file_refs[2];

struct test_backing {
	struct vma_backing backing;
	int references;
};

static void backing_get(struct vma_backing *base)
{
	struct test_backing *backing;

	backing = container_of(base, struct test_backing, backing);
	CHECK(backing->references > 0);
	backing->references++;
}

static void backing_put(struct vma_backing *base)
{
	struct test_backing *backing;

	backing = container_of(base, struct test_backing, backing);
	CHECK(backing->references > 0);
	backing->references--;
}

struct vfs_file *vfs_file_get(struct vfs_file *file)
{
	int index = file - files;

	CHECK(index >= 0 && index < 2);
	file_refs[index]++;
	return file;
}

void vfs_file_put(struct vfs_file *file)
{
	int index = file - files;

	CHECK(index >= 0 && index < 2);
	CHECK(file_refs[index] > 0);
	file_refs[index]--;
}

static const struct vm_area *area_at(const struct vma_set *set, int index)
{
	list_t node = set->areas.next;

	while (index-- && node != &set->areas)
		node = node->next;
	CHECK(node != &set->areas);
	return list_entry(node, struct vm_area, node);
}

static void test_order_and_merge(void)
{
	struct vma_set set;
	const struct vm_area *area;

	vma_set_init(&set);
	CHECK(vma_insert(&set, 0x4000, 0x6000, LINUX_PROT_READ,
			 LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_MMAP, 0,
			 0) == 0);
	CHECK(vma_insert(&set, 0x1000, 0x2000, LINUX_PROT_READ,
			 LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_MMAP, 0,
			 0) == 0);
	CHECK(vma_insert(&set, 0x2000, 0x4000, LINUX_PROT_READ,
			 LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_MMAP, 0,
			 0) == 0);
	CHECK(vma_count(&set) == 1);
	area = area_at(&set, 0);
	CHECK(area->start == 0x1000 && area->end == 0x6000);
	CHECK(vma_insert(&set, 0x5000, 0x7000, LINUX_PROT_READ,
			 LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_MMAP, 0,
			 0) < 0);
	CHECK(vma_find(&set, 0x3000) == area);
	CHECK(!vma_find(&set, 0x7000));
	vma_set_destroy(&set);
}

static void test_gap_selection(void)
{
	struct vma_set set;
	uint64 address;

	vma_set_init(&set);
	CHECK(vma_insert(&set, 0x2000, 0x4000, LINUX_PROT_READ,
			 LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_MMAP, 0,
			 0) == 0);
	CHECK(vma_insert(&set, 0x8000, 0xa000, LINUX_PROT_READ,
			 LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_MMAP, 0,
			 0) == 0);
	CHECK(vma_find_gap(&set, 0x1000, 0x10000, 0, 0x2000,
			   &address) == 0);
	CHECK(address == 0xe000);
	CHECK(vma_find_gap(&set, 0x1000, 0x10000, 0x5000, 0x2000,
			   &address) == 0);
	CHECK(address == 0x5000);
	CHECK(vma_range_free(&set, 0x4000, 0x8000));
	CHECK(!vma_range_free(&set, 0x3000, 0x5000));
	CHECK(!vma_range_mapped(&set, 0x3000, 0x9000));
	vma_set_destroy(&set);
}

static void test_aligned_gap_selection(void)
{
	struct vma_set set;
	uint64 address;

	vma_set_init(&set);
	CHECK(vma_find_gap_aligned(&set, 0x1000, 0x800000, 0x3000,
				   0x3000, 0x200000, 0x1000,
				   &address) == 0);
	CHECK(address == 0x201000);
	CHECK(vma_insert(&set, 0x201000, 0x205000, LINUX_PROT_READ,
			 LINUX_MAP_PRIVATE, VMA_ANONYMOUS, VMA_MMAP,
			 0, 0) == 0);
	CHECK(vma_find_gap_aligned(&set, 0x1000, 0x800000, 0x3000,
				   0x3000, 0x200000, 0x1000,
				   &address) == 0);
	CHECK(address == 0x601000);
	vma_set_destroy(&set);

	vma_set_init(&set);
	CHECK(vma_find_gap_aligned(&set, 0x20000000, 0x40000000,
				   0x20000000, 0x2000, 0x200000,
				   0x20000000, &address) == 0);
	CHECK(address == 0x20000000);
	CHECK(vma_find_gap_aligned(&set, 0x1000, 0x800000, 0,
				   0x1000, 0x3000, 0, &address) < 0);
	vma_set_destroy(&set);
}

static void test_elf_overlap(void)
{
	struct vma_set set;
	const struct vm_area *area;

	vma_set_init(&set);
	CHECK(vma_insert_elf(&set, 0x10000, 0x13000,
			     LINUX_PROT_READ | LINUX_PROT_EXEC,
			     &files[0], 0) == 0);
	CHECK(vma_insert_elf(&set, 0x12000, 0x15000,
			     LINUX_PROT_READ | LINUX_PROT_WRITE,
			     &files[0], 0x2000) == 0);
	CHECK(vma_count(&set) == 3 && file_refs[0] == 3);
	area = area_at(&set, 1);
	CHECK(area->start == 0x12000 && area->end == 0x13000);
	CHECK(area->protection == (LINUX_PROT_READ | LINUX_PROT_WRITE |
				   LINUX_PROT_EXEC));
	CHECK(area->origin == VMA_FILE_BACKED && area->offset == 0x2000);
	CHECK(area->file_length == 0x1000);
	vma_set_destroy(&set);
	CHECK(!file_refs[0]);

	vma_set_init(&set);
	CHECK(vma_insert_elf(&set, 0x10000, 0x13000,
			     LINUX_PROT_READ | LINUX_PROT_EXEC,
			     &files[0], 0) == 0);
	CHECK(vma_insert_elf(&set, 0x12000, 0x15000,
			     LINUX_PROT_READ | LINUX_PROT_WRITE,
			     &files[0], 0x8000) == 0);
	CHECK(vma_count(&set) == 3 && file_refs[0] == 2);
	area = area_at(&set, 1);
	CHECK(area->origin == VMA_ANONYMOUS && !area->file);
	CHECK(!area->file_length);
	CHECK(area->protection == (LINUX_PROT_READ | LINUX_PROT_WRITE |
				   LINUX_PROT_EXEC));
	vma_set_destroy(&set);
	CHECK(!file_refs[0]);
}

static void test_split_and_protect(void)
{
	struct vma_set set;
	const struct vm_area *area;

	vma_set_init(&set);
	CHECK(vma_insert(&set, 0x10000, 0x15000, LINUX_PROT_READ,
			 LINUX_MAP_PRIVATE, VMA_FILE_BACKED, VMA_MMAP,
			 &files[0], 0x2000) == 0);
	CHECK(file_refs[0] == 1);
	CHECK(vma_protect(&set, 0x11000, 0x14000,
			  LINUX_PROT_READ | LINUX_PROT_WRITE) == 0);
	CHECK(vma_count(&set) == 3 && file_refs[0] == 3);
	area = area_at(&set, 0);
	CHECK(area->start == 0x10000 && area->end == 0x11000 &&
	      area->offset == 0x2000 && area->file_length == 0x1000);
	area = area_at(&set, 1);
	CHECK(area->start == 0x11000 && area->end == 0x14000 &&
	      area->offset == 0x3000 && area->file_length == 0x3000);
	area = area_at(&set, 2);
	CHECK(area->start == 0x14000 && area->end == 0x15000 &&
	      area->offset == 0x6000 && area->file_length == 0x1000);
	CHECK(vma_protect(&set, 0x10000, 0x15000,
			  LINUX_PROT_READ) == 0);
	CHECK(vma_count(&set) == 1 && file_refs[0] == 1);
	CHECK(vma_unmap(&set, 0x12000, 0x13000) == 0);
	CHECK(vma_count(&set) == 2 && file_refs[0] == 2);
	CHECK(vma_range_free(&set, 0x12000, 0x13000));
	area = area_at(&set, 1);
	CHECK(area->start == 0x13000 && area->offset == 0x5000 &&
	      area->file_length == 0x2000);
	CHECK(vma_unmap(&set, 0x10000, 0x15000) == 0);
	CHECK(!vma_count(&set) && !file_refs[0]);
	vma_set_destroy(&set);
}

static void test_clone_and_move(void)
{
	struct vma_set clone, moved, source;

	vma_set_init(&source);
	vma_set_init(&clone);
	vma_set_init(&moved);
	CHECK(vma_insert(&source, 0x20000, 0x22000,
			 LINUX_PROT_READ, LINUX_MAP_PRIVATE,
			 VMA_FILE_BACKED, VMA_ELF, &files[1], 0) == 0);
	CHECK(vma_insert(&source, 0x30000, 0x32000,
			 LINUX_PROT_READ | LINUX_PROT_WRITE,
			 LINUX_MAP_PRIVATE | LINUX_MAP_ANONYMOUS,
			 VMA_ANONYMOUS, VMA_STACK, 0, 0) == 0);
	CHECK(vma_set_clone(&clone, &source) == 0);
	CHECK(vma_count(&clone) == 2 && file_refs[1] == 2);
	CHECK(vma_range_mapped(&clone, 0x20000, 0x22000));
	vma_set_move(&moved, &clone);
	CHECK(!vma_count(&clone) && vma_count(&moved) == 2);
	vma_set_destroy(&source);
	vma_set_destroy(&moved);
	CHECK(!file_refs[1]);
}

static void test_shared_backing(void)
{
	struct test_backing backing = {
		.backing = {
			.get = backing_get,
			.put = backing_put,
		},
		.references = 1,
	};
	struct vma_set clone, source;
	const struct vm_area *area;

	vma_set_init(&source);
	vma_set_init(&clone);
	CHECK(vma_insert_backed(&source, 0x40000, 0x44000,
				LINUX_PROT_READ | LINUX_PROT_WRITE,
				LINUX_MAP_SHARED, VMA_MMAP,
				&backing.backing, 0) == 0);
	CHECK(backing.references == 2);
	CHECK(vma_protect(&source, 0x41000, 0x43000,
			  LINUX_PROT_READ) == 0);
	CHECK(vma_count(&source) == 3 && backing.references == 4);
	area = area_at(&source, 1);
	CHECK(area->backing == &backing.backing && area->offset == 0x1000);
	area = area_at(&source, 2);
	CHECK(area->backing == &backing.backing && area->offset == 0x3000);
	CHECK(vma_set_clone(&clone, &source) == 0);
	CHECK(vma_count(&clone) == 3 && backing.references == 7);
	CHECK(vma_unmap(&source, 0x41000, 0x42000) == 0);
	area = area_at(&source, 1);
	CHECK(area->start == 0x42000 && area->offset == 0x2000);
	vma_set_destroy(&source);
	vma_set_destroy(&clone);
	CHECK(backing.references == 1);
	backing.backing.put(&backing.backing);
	CHECK(!backing.references);
}

int main(void)
{
	test_order_and_merge();
	test_gap_selection();
	test_aligned_gap_selection();
	test_elf_overlap();
	test_split_and_protect();
	test_clone_and_move();
	test_shared_backing();
	puts("VMA_OK");
	return EXIT_SUCCESS;
}
