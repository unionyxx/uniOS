#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Streaming playback status returned by SYS_SOUND_STATUS. */
typedef struct sound_status
{
    uint64_t played_bytes; /* source PCM bytes consumed by the card since stream open */
    uint64_t queued_bytes; /* source PCM bytes buffered in the kernel, not yet played */
    uint32_t sample_rate;  /* stream format passed to SYS_SOUND_STREAM_OPEN */
    uint8_t channels;
    uint8_t bits_per_sample;
    uint8_t active;  /* stream is open */
    uint8_t playing; /* card is clocking out samples right now */
    uint8_t paused;  /* stream paused (latched pauses count too) */
    uint8_t owned;   /* stream belongs to the calling process's group */
    uint8_t card_present;
    uint8_t reserved[2];
} sound_status_t;

#define SOUND_VOLUME_MAX 100u

#ifdef __cplusplus
}
#endif
