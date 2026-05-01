#pragma once

#include <stdint.h>
#include <uapi/fs.h>

uint32_t storage_get_mode();
void storage_set_mode(uint32_t mode);
bool storage_reads_allowed();
bool storage_writes_allowed();
