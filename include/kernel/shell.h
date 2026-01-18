#pragma once
#include <stdint.h>
#include <boot/limine.h>

void shell_init(struct limine_framebuffer* fb);
void shell_process_char(char c);
void shell_tick();  // Call periodically for cursor blinking
