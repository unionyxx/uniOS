#pragma once
#include <stdbool.h>
#include <stdint.h>

bool mp3_open(const char *filename, uint8_t **data, uint32_t *data_size, uint32_t *sample_rate, uint32_t *channels);
