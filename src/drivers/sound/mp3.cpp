#include <drivers/sound/mp3.h>
#include <kernel/debug.h>

// MP3 decoder placeholder.
// TODO: Implement MP3 decoding (e.g., minimp3).
bool mp3_open(const char* filename, uint8_t** data, uint32_t* data_size, uint32_t* sample_rate, uint32_t* channels) {
    (void)filename;
    (void)data;
    (void)data_size;
    (void)sample_rate;
    (void)channels;

    DEBUG_WARN("mp3 decoding is not implemented yet");
    return false;
}
