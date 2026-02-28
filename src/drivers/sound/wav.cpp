#include <drivers/sound/wav.h>
#include <kernel/debug.h>
#include <kernel/fs/vfs.h>
#include <kernel/syscall.h>
#include <kernel/mm/heap.h>

bool wav_open(const char* filename, uint8_t** data, uint32_t* data_size, uint32_t* sample_rate, uint32_t* channels, uint8_t** buffer_out) {
    VNodeStat st;
    if (vfs_stat(filename, &st) < 0) {
        DEBUG_ERROR("%s: failed to stat wav file", filename);
        return false;
    }

    if (st.size <= 0xFF) {
        DEBUG_ERROR("%s: invalid or corrupted wav file", filename);
        return false;
    }

    int fd = vfs_open(filename, O_RDONLY);
    if (fd < 0) {
        DEBUG_ERROR("%s: vfs_open failed", filename);
        return false;
    }

    uint8_t* file_data = (uint8_t*)malloc(st.size);
    if (!file_data) {
        vfs_close(fd);
        DEBUG_ERROR("%s: out of memory", filename);
        return false;
    }

    int64_t bytes_read = vfs_read(fd, file_data, st.size);
    vfs_close(fd);

    if (bytes_read < (int64_t)st.size) {
        free(file_data);
        DEBUG_ERROR("%s: failed to read wav file", filename);
        return false;
    }

    WavHeader wav_header;
    wav_header.riff_descriptor = (WavRiffDescriptor*)file_data;
    wav_header.fmt_chunk = (WavFmtChunk*)(file_data + sizeof(WavRiffDescriptor));

    char* wave = (char*)wav_header.riff_descriptor->wave;
    if (wave[0] != 'W' || wave[1] != 'A' || wave[2] != 'V' || wave[3] != 'E') {
        free(file_data);
        DEBUG_ERROR("%s: invalid wav header", filename);
        return false;
    }

    // Look for data chunk.
    WavFmtChunk* fmt_chunk = wav_header.fmt_chunk;
    wav_header.data_chunk = nullptr;
    for (uint32_t i = 0; i < 0xFF && (sizeof(WavRiffDescriptor) + i + 4 <= st.size); i++) {
        uint8_t* byte = ((uint8_t*)fmt_chunk) + i;
        if (byte[0] == 'd' && byte[1] == 'a' && byte[2] == 't' && byte[3] == 'a') {
            wav_header.data_chunk = (WavDataChunk*)byte;
            break;
        }
    }

    WavDataChunk* data_chunk = wav_header.data_chunk;
    if (!data_chunk) {
        free(file_data);
        DEBUG_ERROR("%s: failed to find data chunk", filename);
        return false;
    }

    if (fmt_chunk->audio_format == 0 || fmt_chunk->samples == 0 || fmt_chunk->channels == 0 || data_chunk->data_size == 0) {
        free(file_data);
        DEBUG_ERROR("%s: invalid wav data", filename);
        return false;
    }

    // Only PCM format supported
    if (fmt_chunk->audio_format != 1) {
        free(file_data);
        DEBUG_ERROR("%s: non-pcm format is not supported", filename);
        return false;
    }

    // Only 16-bit is supported
    if (fmt_chunk->bits_per_sample != 16) {
        free(file_data);
        DEBUG_ERROR("%s: only 16-bit data is supported", filename);
        return false;
    }

    // Set output data.
    *data = (uint8_t*)&data_chunk->data_;
    *data_size = data_chunk->data_size;
    *sample_rate = fmt_chunk->samples;
    *channels = fmt_chunk->channels;
    *buffer_out = file_data;

    return true;
}
