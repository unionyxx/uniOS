#pragma once
#include <stdint.h>
#include <stdbool.h>

struct Mp3DecodingData {
    uint8_t* input_buffer; // Pointer to MP3 byte array.
    uint64_t input_buffer_size; // MP3 byte array size.

    uint8_t* output_buffer; // Pointer to PCM output byte array.
    uint64_t output_buffer_size; // PCM byte array size.

    uint64_t output_buffer_offset; // Offset to next PCM byte. Only for internal use.

    uint32_t sample_rate; // Output sample rate.
    uint32_t channels; // Output channels.
};

bool mp3_open(const char* filename, uint8_t** data, uint32_t* data_size, uint32_t* sample_rate, uint32_t* channels);
