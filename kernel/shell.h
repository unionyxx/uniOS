#pragma once
#include <stdint.h>
#include "limine.h"

void shell_init(struct limine_framebuffer* fb);
void shell_process_char(char c);
