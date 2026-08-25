#include <anon_mapping.h>
#include <debug.h>
#include <list.h>
#include <page_cache.h>
#include <palloc.h>
#include <riscv.h>
#include <sleeplock.h>
#include <vma.h>

struct anon_mapping_page {
	struct list node;
	uint64 offset;
	void *page;
};

struct anon_mapping {
	struct vma_backing backing;
	struct sleeplock lock;
	struct list pages;
	uint32 references;
};

static struct anon_mapping *to_anon_mapping(struct vma_backing *backing)
{
	return container_of(backing, struct anon_mapping, backing);
}

static void anon_mapping_get(struct vma_backing *backing)
{
	struct anon_mapping *mapping = to_anon_mapping(backing);
	uint32 references;

	references = __atomic_fetch_add(&mapping->references, 1,
					__ATOMIC_ACQ_REL);
	if (!references || references == (uint32)-1)
		PANIC("invalid anonymous mapping reference");
}

static void anon_mapping_put(struct vma_backing *backing)
{
	struct anon_mapping *mapping = to_anon_mapping(backing);
	struct anon_mapping_page *page;
	uint32 references;
	list_t next, node;

	references = __atomic_fetch_sub(&mapping->references, 1,
					__ATOMIC_ACQ_REL);
	if (!references)
		PANIC("invalid anonymous mapping release");
	if (references != 1)
		return;
	for (node = mapping->pages.next; node != &mapping->pages;
	     node = next) {
		next = node->next;
		page = list_entry(node, struct anon_mapping_page, node);
		list_remove(node);
		pfree(page->page);
		free(page);
	}
	free(mapping);
}

struct vma_backing *anon_mapping_create(void)
{
	struct anon_mapping *mapping = malloc(sizeof(*mapping));

	if (!mapping)
		return 0;
	mapping->backing.get = anon_mapping_get;
	mapping->backing.put = anon_mapping_put;
	sleeplock_init(&mapping->lock, "anonymous mapping");
	list_init(&mapping->pages);
	mapping->references = 1;
	return &mapping->backing;
}

int anon_mapping_get_page(struct vma_backing *backing, uint64 offset,
			  void **result)
{
	struct anon_mapping *mapping;
	struct anon_mapping_page *entry;
	void *page;
	list_t node;

	if (!backing || !result || offset % PGSIZE)
		return -1;
	mapping = to_anon_mapping(backing);
	sleeplock_acquire(&mapping->lock);
	for (node = mapping->pages.next; node != &mapping->pages;
	     node = node->next) {
		entry = list_entry(node, struct anon_mapping_page, node);
		if (entry->offset == offset) {
			if (palloc_get(entry->page) < 0)
				goto failed;
			*result = entry->page;
			sleeplock_release(&mapping->lock);
			return 0;
		}
	}
	entry = malloc(sizeof(*entry));
	page = palloc_zero();
	if ((!entry || !page) && page_cache_reclaim(1)) {
		if (!entry)
			entry = malloc(sizeof(*entry));
		if (!page)
			page = palloc_zero();
	}
	if (!entry || !page)
		goto failed_alloc;
	list_init(&entry->node);
	entry->offset = offset;
	entry->page = page;
	list_insert_after(&mapping->pages, &entry->node);
	if (palloc_get(page) < 0) {
		list_remove(&entry->node);
		goto failed_alloc;
	}
	*result = page;
	sleeplock_release(&mapping->lock);
	return 0;

failed_alloc:
	if (page)
		pfree(page);
	if (entry)
		free(entry);
failed:
	sleeplock_release(&mapping->lock);
	return -1;
}
