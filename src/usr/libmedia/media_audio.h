#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Probed PCM stream: data_start/data_size are the absolute file offset and
// byte length of the payload, not chunk-relative values.
struct media_audio_info
{
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint64_t data_start;
    uint64_t data_size;
};

// Probe a RIFF/WAV stream from a header window held in memory. Parsing reads
// stay within the first `size` bytes of `data` (a streaming caller may hold
// fewer bytes than the file), while the payload bounds check uses `file_size`
// (reject data_start + data_size > file_size). Accepts only PCM 16-bit
// mono/stereo at 8000-48000 Hz. Returns false on unknown format, malformed
// input, or an out-of-bounds payload with a serial log naming the field; out
// is untouched on failure.
bool media_audio_probe(const void *data, size_t size, uint64_t file_size, struct media_audio_info *out);

#ifdef __cplusplus
}
#endif
