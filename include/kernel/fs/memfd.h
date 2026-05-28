#pragma once

#include <stddef.h>

struct VNode;
extern struct VNodeOps memfd_ops;

struct VNode *memfd_create_vnode(const char *name);
bool is_memfd_vnode(struct VNode *node);
void *memfd_get_page(struct VNode *node, size_t page_idx);
