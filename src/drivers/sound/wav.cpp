#include <drivers/sound/wav.h>
#include <kernel/debug.h>
#include <kernel/fs/unifs.h>

bool wav_open(const char* filename, uint8_t** data, uint32_t* data_size, uint32_t* sample_rate, uint32_t* channels) {
    UniFSFile file;
    if (!unifs_open_into(filename, &file)) {
        DEBUG_ERROR("%s: unifs_open_into failed", filename);
        return false;
    }

    if (file.size <= 0xFF) {
        DEBUG_ERROR("%s: invalid or corrupted wav file", filename);
        return false;
    }

    WavHeader wav_header;
    wav_header.riff_descriptor = (WavRiffDescriptor*)(uint64_t)file.data;
    wav_header.fmt_chunk = (WavFmtChunk*)((uint64_t)file.data + sizeof(WavRiffDescriptor));

    char* wave = (char*)wav_header.riff_descriptor->wave;
    if (wave[0] != 'W' || wave[1] != 'A' || wave[2] != 'V' || wave[3] != 'E') {
        DEBUG_ERROR("%s: invalid wav header", filename);
        return false;
    }

    // Look for data chunk.
    WavFmtChunk* fmt_chunk = wav_header.fmt_chunk;
    for (uint8_t i = 0; i < 0xFF; i++) {
        uint8_t* byte = ((uint8_t*)fmt_chunk) + i;
        if (byte[0] == 'd' && byte[1] == 'a' && byte[2] == 't' && byte[3] == 'a') {
            wav_header.data_chunk = (WavDataChunk*)byte;
            break;
        }
    }

    WavDataChunk* data_chunk = wav_header.data_chunk;
    if (!data_chunk) {
        DEBUG_ERROR("%s: failed to find data chunk", filename);
        return false;
    }

    DEBUG_INFO("%s: format=%d | sample_rate=%d | bps=%d | channels=%d | data_size=%d", filename, fmt_chunk->audio_format, fmt_chunk->samples, fmt_chunk->bits_per_sample, fmt_chunk->channels, data_chunk->data_size);
    if (fmt_chunk->audio_format == 0 || fmt_chunk->samples == 0 || fmt_chunk->channels == 0 || data_chunk->data_size == 0) {
        DEBUG_ERROR("%s: invalid wav data", filename);
        return false;
    }

    // Only PCM format supported
    if (fmt_chunk->audio_format != 1) {
        DEBUG_ERROR("%s: non-pcm format is not supported", filename);
        return false;
    }

    // Only 16-bit is supported
    if (fmt_chunk->bits_per_sample != 16) {
        DEBUG_ERROR("%s: only 16-bit data is supported", filename);
        return false;
    }

    // Set output data.
    *data = &data_chunk->data_;
    *data_size = data_chunk->data_size;

    *sample_rate = fmt_chunk->samples;
    *channels = fmt_chunk->channels;

    return true;
}
