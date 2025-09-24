// kernel/slab.h
#pragma once
#include "types.h"

struct kmem_cache;

struct kmem_cache *kmem_cache_create(const char *name, uint objsize,
                                     void (*ctor)(void *), void (*dtor)(void *),
                                     uint align);
void kmem_cache_destroy(struct kmem_cache *c);
void *kmem_cache_alloc(struct kmem_cache *c);
void  kmem_cache_free (struct kmem_cache *c, void *obj);

void  kmalloc_init(void);
void *kmalloc(uint size);
void  kfree_slab(void *p);
void  kmalloc_stats(void);
