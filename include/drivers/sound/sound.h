#pragma once
#include <stdint.h>
#include <stdbool.h>

#define SOUND_NONE 0
#define SOUND_AC97 1
#define SOUND_HD_AUDIO 2

bool sound_is_initialized();
uint8_t sound_get_card_type();

bool sound_is_paused();
bool sound_is_playing();

void sound_init();
void sound_reset();

void sound_set_volume(uint8_t volume);
uint8_t sound_get_volume();

void sound_set_channels(uint8_t channels);
void sound_set_bits_per_sample(uint8_t bits_per_sample);
void sound_set_sample_rate(uint32_t sample_rate);

void sound_play_mp3_file(const char* filename);
void sound_play_wav_file(const char* filename);
void sound_play_pcm_file(const char* filename);

void sound_play(uint8_t* data, uint32_t size);
void sound_resume();
void sound_pause();
void sound_stop();

void sound_poll();

uint32_t sound_get_played_bytes();
