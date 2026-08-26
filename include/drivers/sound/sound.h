#pragma once
#include <stdbool.h>
#include <stdint.h>

#define SOUND_NONE 0
#define SOUND_AC97 1
#define SOUND_HD_AUDIO 2

bool sound_is_initialized();
uint8_t sound_get_card_type();

bool sound_is_paused();
bool sound_is_playing();

void sound_init();
void sound_stream_ring_init();
void sound_reset();

void sound_set_volume(uint8_t volume);
uint8_t sound_get_volume();

void sound_set_channels(uint8_t channels);
void sound_set_bits_per_sample(uint8_t bits_per_sample);
void sound_set_sample_rate(uint32_t sample_rate);

void sound_play_mp3_file(const char *filename);
void sound_play_wav_file(const char *filename);
void sound_play_pcm_file(const char *filename);

void sound_play(uint8_t *data, uint32_t size);
void sound_resume();
void sound_pause();
void sound_stop();

void sound_poll();

// Streaming playback: userspace pushes decoded PCM incrementally instead of
// buffering a whole file. The dispatcher keeps a kernel ring, starts the card
// once enough is queued, and refills DMA from the ring as entries are
// consumed. 16-bit samples only; mono is upmixed to stereo for cards that
// DMA stereo pairs.
struct sound_status;
bool sound_stream_open(uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample);
int64_t sound_stream_write(const void *data, uint32_t len);
void sound_stream_end();
void sound_stream_stop();
bool sound_stream_active();
bool sound_stream_status(struct sound_status *out);

void sound_fill_dma_buffer(uint8_t *dst, uint8_t *src, uint32_t src_size, uint32_t offset, uint32_t chunk_size);

uint32_t sound_get_played_bytes();
