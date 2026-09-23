/*
 * Intrusive circular doubly linked lists. An empty head points to itself;
 * removed entries are reinitialized. Callers own entries and provide
 * synchronization for both traversal and mutation.
 */
#ifndef __CAFFEINIX_KERNEL_LIST_H
#define __CAFFEINIX_KERNEL_LIST_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct list {
        struct list *prev;
        struct list *next;
}*list_t;

#define container_of(ptr, type, member) \
    ((type *)((char *)(ptr) - (unsigned long)(&((type *)0)->member)))

#define list_entry(ptr, type, member) \
    container_of(ptr, type, member)

/**
 * list_init() - Initialize an empty head or detached entry
 * @l: Caller-owned list head or unlinked node.
 *
 * Context: Before publication or with caller-provided serialization.
 */
static inline void list_init(list_t l){
    l->prev = l->next = l;
}

/**
 * list_insert_after() - Link an entry after an existing node
 * @l: Linked node or initialized list head.
 * @n: Detached entry retained by the caller.
 *
 * Context: Caller serializes list traversal and mutation.
 */
static inline void list_insert_after(list_t l, list_t n){
    l->next->prev = n;
    n->next = l->next;
    n->prev = l;
    l->next = n;
}

/**
 * list_insert_before() - Link an entry before an existing node
 * @l: Linked node or initialized list head.
 * @n: Detached entry retained by the caller.
 *
 * Context: Caller serializes list traversal and mutation.
 */
static inline void list_insert_before(list_t l, list_t n){
    l->prev->next = n;
    n->prev = l->prev;
    n->next = l;
    l->prev = n;
}

/**
 * list_remove() - Detach and reinitialize an entry
 * @l: Linked entry, or an already self-linked node.
 *
 * Does not free the enclosing object.
 *
 * Context: Caller serializes list traversal and mutation.
 */
static inline void list_remove(list_t l){
    l->next->prev = l->prev;
    l->prev->next = l->next;

    list_init(l);
}

#ifdef __cplusplus
}
#endif

#endif
