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

static inline void list_init(list_t l){
    l->prev = l->next = l;
}

static inline void list_insert_after(list_t l, list_t n){
    l->next->prev = n;
    n->next = l->next;
    n->prev = l;
    l->next = n;
}

static inline void list_insert_before(list_t l, list_t n){
    l->prev->next = n;
    n->prev = l->prev;
    n->next = l;
    l->prev = n;
}

static inline void list_remove(list_t l){
    l->next->prev = l->prev;
    l->prev->next = l->next;

    list_init(l);
}

#ifdef __cplusplus
}
#endif

#endif