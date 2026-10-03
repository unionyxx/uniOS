#include "media_audio.h"

#include "log.h"

// Canonical WAV prologue this parser reads: a 12-byte RIFF descriptor, then
// the fmt chunk at offset 12 whose fields end at offset 36 (audio format at
// +8, channels +10, sample rate +12, bits per sample +22). The data chunk
// that follows is located by the scan below, so its position is not fixed.
#define WAV_RIFF_BYTES 12
#define WAV_FMT_AUDIO_FORMAT (WAV_RIFF_BYTES + 8)
#define WAV_FMT_CHANNELS (WAV_RIFF_BYTES + 10)
#define WAV_FMT_SAMPLE_RATE (WAV_RIFF_BYTES + 12)
#define WAV_FMT_BITS (WAV_RIFF_BYTES + 22)
#define WAV_PROBE_MIN 36

// Same scan window as the retired libc parser: 0xFF bytes from the fmt chunk
// onward, 8 readable bytes required at each candidate (the "data" magic plus
// the 32-bit size that follows it).
#define WAV_SCAN_WINDOW 0xFF

static uint16_t rd_u16le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd_u32le(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool magic_is(const uint8_t *p, const char magic[4])
{
    return p[0] == (uint8_t)magic[0] && p[1] == (uint8_t)magic[1] && p[2] == (uint8_t)magic[2] &&
           p[3] == (uint8_t)magic[3];
}

bool media_audio_probe(const void *data, size_t size, uint64_t file_size, struct media_audio_info *out)
{
    const uint8_t *p = (const uint8_t *)data;

    if (size < WAV_PROBE_MIN) {
        LOG_ERROR("media_audio", "size: buffer holds %llu bytes, the parser must read %u", (unsigned long long)size,
                  WAV_PROBE_MIN);
        return false;
    }

    if (!magic_is(p, "RIFF")) {
        LOG_ERROR("media_audio", "riff: magic %02x%02x%02x%02x is not RIFF", p[0], p[1], p[2], p[3]);
        return false;
    }
    if (!magic_is(p + 8, "WAVE")) {
        LOG_ERROR("media_audio", "wave: magic %02x%02x%02x%02x is not WAVE", p[8], p[9], p[10], p[11]);
        return false;
    }

    const uint16_t audio_format = rd_u16le(p + WAV_FMT_AUDIO_FORMAT);
    const uint16_t channels = rd_u16le(p + WAV_FMT_CHANNELS);
    const uint32_t sample_rate = rd_u32le(p + WAV_FMT_SAMPLE_RATE);
    const uint16_t bits_per_sample = rd_u16le(p + WAV_FMT_BITS);

    if (audio_format != 1) {
        LOG_ERROR("media_audio", "audio_format: %u is not PCM", audio_format);
        return false;
    }
    if (channels < 1 || channels > 2) {
        LOG_ERROR("media_audio", "channels: %u is not mono or stereo", channels);
        return false;
    }
    if (sample_rate < 8000 || sample_rate > 48000) {
        LOG_ERROR("media_audio", "sample_rate: %u is outside 8000-48000 Hz", sample_rate);
        return false;
    }
    if (bits_per_sample != 16) {
        LOG_ERROR("media_audio", "bits_per_sample: %u is not 16", bits_per_sample);
        return false;
    }

    // Scan reads are bounded by the buffer window, not the file, so a
    // streaming caller's header window smaller than the file still parses.
    uint64_t data_off = 0;
    bool found = false;
    for (uint32_t i = 0; i < WAV_SCAN_WINDOW && (size_t)(WAV_RIFF_BYTES + i + 8) <= size; i++) {
        if (magic_is(p + WAV_RIFF_BYTES + i, "data")) {
            data_off = WAV_RIFF_BYTES + i;
            found = true;
            break;
        }
    }
    if (!found) {
        LOG_ERROR("media_audio", "data: no data chunk in the %u-byte scan window", WAV_SCAN_WINDOW);
        return false;
    }

    const uint64_t data_start = data_off + 8;
    const uint64_t data_size = rd_u32le(p + data_off + 4);

    // data_size is attacker-controlled and the payload is not part of the
    // header window, so the bounds check runs against the file size.
    if (data_start + data_size > file_size) {
        LOG_ERROR("media_audio", "data_size: %llu payload bytes at offset %llu run past the %llu-byte file",
                  (unsigned long long)data_size, (unsigned long long)data_start, (unsigned long long)file_size);
        return false;
    }

    out->sample_rate = sample_rate;
    out->channels = channels;
    out->bits_per_sample = bits_per_sample;
    out->data_start = data_start;
    out->data_size = data_size;
    return true;
}
