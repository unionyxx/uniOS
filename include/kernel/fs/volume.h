#pragma once

#include <uapi/fs.h>

void volume_reset();
bool volume_register(const char *display_name, const char *mount_path, const char *source_device, uint32_t flags);
int volume_list(VolumeInfo *out, int max_count);
