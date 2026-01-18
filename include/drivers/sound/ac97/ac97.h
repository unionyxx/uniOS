#pragma once
#include <stdint.h>
#include <stdbool.h>

#include <drivers/bus/pci/pci.h>
#include <kernel/mm/vmm.h>

// NAM registers.
#define AC97_NAM_RESET 0x00
#define AC97_NAM_CAPABILITES 0x00
#define AC97_NAM_MASTER_VOLUME 0x02
#define AC97_NAM_PCM_OUT_VOLUME 0x18
#define AC97_NAM_PCM_OUT_VOLUME_STEPS 31

#define AC97_NAM_EXTENDED_CAPABILITIES 0x28
#define AC97_NAM_EXTENDED_FEATURES_CONTROL 0x2A

// NAM Extended Capabilities registers.
#define AC97_NAM_VARIABLE_SAMPLE_RATE_FRONT_DAC 0x2C
#define AC97_NAM_VARIABLE_SAMPLE_RATE_SURR_DAC 0x2E
#define AC97_NAM_VARIABLE_SAMPLE_RATE_LFE_DAC 0x30
#define AC97_NAM_VARIABLE_SAMPLE_RATE_LR_ADC 0x32

// PCM Output registers.
#define AC97_NABM_PCM_OUT_BUFFER_BASE_ADDRESS 0x10
#define AC97_NABM_PCM_OUT_CURRENTLY_PROCESSED_ENTRY 0x14
#define AC97_NABM_PCM_OUT_LAST_VALID_ENTRY 0x15
#define AC97_NABM_PCM_OUT_STATUS 0x16
#define AC97_NABM_PCM_OUT_CURRENT_ENTRY_POSITION 0x18
#define AC97_NABM_PCM_OUT_CONTROL 0x1B

#define AC97_NABM_PCM_OUT_CONTROL_STOP 0x0
#define AC97_NABM_PCM_OUT_CONTROL_START 0x1
#define AC97_NABM_PCM_OUT_CONTROL_RESET 0x2

// NABM Global Control offset.
#define AC97_NABM_GLOBAL_CONTROL 0x2C

// NABM Global Control bits.
#define AC97_NABM_GLOBAL_CONTROL_COLD_RESET 1
#define AC97_NABM_GLOBAL_CONTROL_PCM_OUT_CHANNELS 20
#define AC97_NABM_GLOBAL_CONTROL_PCM_OUT_SAMPLES 22

#define AC97_EXTENDED_CAPABILITY_VARIABLE_SAMPLE_RATE 0x1

// Buffer entry definitions.
#define AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE 0x8000
#define AC97_BUFFER_ENTRY_COUNT 32

struct Ac97BufferEntry {
    uint32_t buffer; // Physical address (!) to sound data in memory.
    uint16_t samples; // Number of samples in this buffer.
    uint16_t reserved; // Not needed right now. We don't use interrupts.
};

struct Ac97Device {
    // Sound card data.
    uint64_t nam; // Native Audio Mixer base address.
    uint64_t nabm; // Native Audio Bus Master base address.

    uint16_t capabilities; // Extended capabilities of sound card.

    // Flags.
    bool is_playing; // Is playing something?
    bool is_paused; // Is paused?

    bool free_sound_data_on_stop; // Free sound data memory when playback is finished?

    bool is_initialized; // Is sound card initialized and ready to play?

    uint8_t sound_volume; // Current sound volume.

    // Memory stuff.
    DMAAllocation buffer_entries_dma; // DMA allocation for buffer entries.
    DMAAllocation sound_buffers_dma; // DMA allocation for sound buffers.

    Ac97BufferEntry* buffer_entries; // Buffer entries array (should be virtual address of DMA allocation for buffer entries)

    uint8_t* sound_data; // Byte array of entire PCM sound data (virtual address from filesystem!).
    uint32_t sound_data_size; // Size of entire PCM sound data.

    // Buffer refilling.
    uint32_t current_buffer_entry; // Current buffer entry for refilling.
    uint32_t buffer_entry_offset; // Buffers offset (how many times sound card went back to first buffer since playback start).

    uint32_t played_bytes; // Total amount of played bytes (out of sound_data_size).
};

bool ac97_is_initialized();

bool ac97_is_paused();
bool ac97_is_playing();

void ac97_init();
void ac97_reset();

void ac97_set_volume(uint8_t volume);
uint8_t ac97_get_volume();

void ac97_set_channels(uint8_t channels);
void ac97_set_sample_rate(uint16_t sample_rate);

void ac97_play_wav_file(const char* filename);
void ac97_play_pcm_file(const char* filename);

void ac97_play(uint8_t* data, uint32_t size);
void ac97_resume();
void ac97_pause();
void ac97_stop();

void ac97_poll();

uint32_t ac97_get_played_bytes();
uint32_t ac97_get_stream_position();
