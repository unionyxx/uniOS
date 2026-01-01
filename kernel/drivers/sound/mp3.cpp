#include "mp3.h"

#include "debug.h"
#include "unifs.h"
#include "heap.h"

bool mp3_open(const char* filename, uint8_t** data, uint32_t* data_size, uint32_t* sample_rate, uint32_t* channels) {
    UniFSFile file;
    if (!unifs_open_into(filename, &file)) {
        DEBUG_ERROR("%s: unifs_open_into failed", filename);
        return false;
    }

    uint64_t mp3_size = file.size;
    uint8_t* mp3_bytes = (uint8_t*)(uint64_t)file.data;

    Mp3DecodingData buffer;
    //buffer.output_buffer = malloc(100 * 1024 * 1024);

    *data = buffer.output_buffer;
    *data_size = buffer.output_buffer_size;

    *sample_rate = buffer.sample_rate;
    *channels = buffer.channels;

    // TO-DO: investigate mp3 decoding problems.
    return false;
}
