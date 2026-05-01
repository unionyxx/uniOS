#pragma once
#include <stddef.h>
#include <stdint.h>

struct VNode;

#define UNIFS_MAGIC "UNIFS v1"

void unifs_init(void *start_addr);

struct VNode *unifs_get_root();

bool unifs_is_mounted();

uint64_t unifs_get_total_size();
uint64_t unifs_get_file_count();
uint64_t unifs_get_boot_file_count();
