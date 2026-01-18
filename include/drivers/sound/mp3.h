#pragma once
#include <stdint.h>
#include <stdbool.h>

// MP3 decoder placeholder.
// TODO: Implement MP3 decoding (e.g., minimp3).
bool mp3_open(const char* filename, uint8_t** data, uint32_t* data_size, uint32_t* sample_rate, uint32_t* channels);
