#pragma once
#include <stdint.h>
#include <stdbool.h>

#include <drivers/bus/pci/pci.h>
#include <kernel/mm/vmm.h>

// Control definitions.
#define HDA_GLOBAL_CAPABILITIES 0x0

#define HDA_GLOBAL_CONTROL 0x8
#define HDA_INTERRUPT_CONTROL 0x20

#define HDA_STREAM_SYNCHRONIZATION 0x34
#define HDA_DMA_POSITION_BASE_ADDRESS 0x70

// Control bits.
#define HDA_GLOBAL_CONTROL_STATE 0
#define HDA_GLOBAL_CONTROL_IN_RESET (0 << HDA_GLOBAL_CONTROL_STATE)
#define HDA_GLOBAL_CONTROL_IN_OPERATIONAL_STATE (1 << HDA_GLOBAL_CONTROL_STATE)

// CORB definitions
#define HDA_CORB_BASE_ADDRESS 0x40
#define HDA_CORB_WRITE_POINTER 0x48
#define HDA_CORB_READ_POINTER 0x4A
#define HDA_CORB_CONTROL 0x4C
#define HDA_CORB_SIZE 0x4E

// CORB bits.
#define HDA_CORB_READ_POINTER_RESET 15
#define HDA_CORB_READ_POINTER_CLEAR (0 << HDA_CORB_READ_POINTER_RESET)
#define HDA_CORB_READ_POINTER_IN_RESET (1 << HDA_CORB_READ_POINTER_RESET)

#define HDA_CORB_CONTROL_STATUS 1
#define HDA_CORB_CONTROL_STATUS_STOPPED (0 << HDA_CORB_CONTROL_STATUS)
#define HDA_CORB_CONTROL_STATUS_RUNNING (1 << HDA_CORB_CONTROL_STATUS)

#define HDA_CORB_SIZE_NUMBER_OF_RING_ENTRIES 0

// RIRB definitions.
#define HDA_RIRB_BASE_ADDRESS 0x50
#define HDA_RIRB_WRITE_POINTER 0x58
#define HDA_RIRB_RESPONSE_INTERRUPT_COUNT 0x5A
#define HDA_RIRB_CONTROL 0x5C
#define HDA_RIRB_SIZE 0x5E

// RIRB bits.
#define HDA_RIRB_WRITE_POINTER_RESET 15
#define HDA_RIRB_WRITE_POINTER_IN_RESET (1 << HDA_RIRB_WRITE_POINTER_RESET)

#define HDA_RIRB_CONTROL_STATUS 1
#define HDA_RIRB_CONTROL_STATUS_STOPPED (0 << HDA_RIRB_CONTROL_STATUS)
#define HDA_RIRB_CONTROL_STATUS_RUNNING (1 << HDA_RIRB_CONTROL_STATUS)

#define HDA_RIRB_SIZE_NUMBER_OF_RING_ENTRIES 0

// Immediate Command Interface (for QEMU compatibility - bypasses CORB/RIRB)
#define HDA_IMMEDIATE_COMMAND 0x60      // Immediate Command Output (IC)
#define HDA_IMMEDIATE_RESPONSE 0x64     // Immediate Response Input (IR)
#define HDA_IMMEDIATE_STATUS 0x68       // Immediate Command Status (ICS)
#define HDA_ICS_BUSY (1 << 0)           // ICB - Immediate Command Busy
#define HDA_ICS_VALID (1 << 1)          // IRV - Immediate Result Valid

// Stream descriptor definitions.
#define HDA_STREAM_DESCRIPTOR_BASE 0x80
#define HDA_STREAM_DESCRIPTOR_SIZE 0x20

#define HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_1 0x0
#define HDA_STREAM_DESCRIPTOR_STREAM_CONTROL_2 0x2
#define HDA_STREAM_DESCRIPTOR_STREAM_STATUS 0x3
#define HDA_STREAM_DESCRIPTOR_BUFFER_ENTRY_POSITION 0x4
#define HDA_STREAM_DESCRIPTOR_RING_BUFFER_LENGTH 0x8
#define HDA_STREAM_DESCRIPTOR_LAST_VALID_INDEX 0xC
#define HDA_STREAM_DESCRIPTOR_STREAM_FORMAT 0x12
#define HDA_STREAM_DESCRIPTOR_BDL_BASE_ADDRESS 0x18

// Stream control bits.
#define HDA_STREAM_CONTROL_RESET_REGISTERS 0
#define HDA_STREAM_CONTROL_STREAM_STATUS 1

#define HDA_STREAM_CONTROL_STREAM_STOPPED (0 << HDA_STREAM_CONTROL_STREAM_STATUS)
#define HDA_STREAM_CONTROL_STREAM_RUNNING (1 << HDA_STREAM_CONTROL_STREAM_STATUS)
#define HDA_STREAM_CONTROL_STREAM_IN_RESET (1 << HDA_STREAM_CONTROL_RESET_REGISTERS)

// Buffer entry definitions.
#define HDA_BUFFER_ENTRY_SOUND_BUFFER_SIZE 0x8000 // Not in use for now.
#define HDA_BUFFER_ENTRY_COUNT 32

#define HDA_CORB_ENTRY_COUNT 256
#define HDA_RIRB_ENTRY_COUNT 256

// Command bits.
#define HDA_NODE_COMMAND_DATA 0
#define HDA_NODE_COMMAND_COMMAND 8
#define HDA_NODE_COMMAND_NODE_INDEX 20
#define HDA_NODE_COMMAND_CODEC 28

// Verbs
#define HDA_VERB_GET_PARAMETER 0xF00
#define HDA_VERB_GET_CONNECTION_LIST_ENTRY 0xF02
#define HDA_VERB_GET_PIN_WIDGET_CONFIGURATION 0xF1C

#define HDA_VERB_SET_SELECTED_INPUT 0x701
#define HDA_VERB_SET_POWER_STATE 0x705
#define HDA_VERB_SET_CONVERTER_STREAM 0x706
#define HDA_VERB_SET_PIN_WIDGET_CONTROL 0x707
#define HDA_VERB_SET_EAPD 0x70C
#define HDA_VERB_AFG_NODE_RESET 0x7FF

#define HDA_VERB_SET_STREAM_FORMAT 0x200
#define HDA_VERB_SET_AMPLIFIER_GAIN 0x300

// Node parameters.
#define HDA_NODE_PARAMETER_NODE_COUNT 0x4
#define HDA_NODE_PARAMETER_FUNCTION_GROUP_TYPE 0x5
#define HDA_NODE_PARAMETER_AUDIO_WIDGET_CAPABILITIES 0x9
#define HDA_NODE_PARAMETER_SUPPORTED_PCM_RATES 0xA
#define HDA_NODE_PARAMETER_SUPPORTED_FORMATS 0xB
#define HDA_NODE_PARAMETER_CONNECTION_LIST_LENGTH 0xE
#define HDA_NODE_PARAMETER_OUTPUT_AMPLIFIER_CAPABILITIES 0x12

// Widgets.
#define HDA_WIDGET_AUDIO_OUTPUT 0
#define HDA_WIDGET_AUDIO_INPUT 1
#define HDA_WIDGET_AUDIO_MIXER 2
#define HDA_WIDGET_PIN_COMPLEX 4

#define HDA_WIDGET_AFG 0xAF6

// Pins.
#define HDA_PIN_LINE_OUT 0
#define HDA_PIN_HEADPHONE_OUT 2
#define HDA_PIN_MIC_IN 10
#define HDA_PIN_LINE_IN 8

// Miscs.
#define HDA_MAX_AFG_NODES 48

#define HDA_INVALID 0xFFFFFFFF

struct HdAudioNode {
    uint32_t node;
    uint32_t node_type;

    uint32_t parent_node;
    uint32_t parent_node_type;

    uint32_t supported_rates;
    uint32_t supported_formats;
    uint32_t output_amplifier_capabilities;

    inline void init(uint32_t node, uint32_t node_type, uint32_t parent_node, uint32_t parent_node_type) {
        this->node = node;
        this->node_type = node_type;

        this->parent_node = parent_node;
        this->parent_node_type = parent_node_type;
    }
};

struct HdAudioBufferEntry {
    uint64_t buffer; // Physical address (!) to sound data in memory.
    uint32_t buffer_size; // Sound data size.
    uint16_t reserved; // Not needed right now.
};

struct HdAudioDevice {
    // Flags.
    bool is_playing; // Is playing something?
    bool is_paused; // Is paused?

    bool free_sound_data_on_stop; // Free sound data memory when playback is finished?

    bool is_initialized; // Is sound card initialized and ready to play?

    uint8_t sound_volume; // Current sound volume.

    // Stream parameters.
    uint8_t channels; // Sound buffer channels.
    uint8_t bits_per_sample; // Sound buffer bits per sample.

    uint32_t sample_rate; // Sound buffer sample rate.

    // Sound card data.
    uint64_t base; // PCI BAR0 virtual address for communication.
    uint64_t output_stream; // Address of output stream.

    uint32_t codec; // CODEC ID.

    // Audio Function Group node.
    HdAudioNode afg;

    // AFG nodes.
    HdAudioNode nodes[HDA_MAX_AFG_NODES];
    uint32_t node_count;

    // Memory stuff.
    DMAAllocation buffer_entries_dma; // DMA allocation for buffer entries.
    DMAAllocation sound_buffers_dma; // DMA allocation for sound buffers.

    HdAudioBufferEntry* buffer_entries; // Buffer entries array (should be virtual address of DMA allocation for buffer entries)

    uint8_t* sound_data; // Byte array of entire PCM sound data (virtual address from filesystem!).
    uint32_t sound_data_size; // Size of entire PCM sound data.

    // CORB/RIRB for hardware command transfer (DMA-based)
    DMAAllocation corb_dma;     // DMA allocation for CORB entries.
    DMAAllocation rirb_dma;     // DMA allocation for RIRB entries.

    uint32_t* corb;             // Virtual address of CORB entries.
    uint32_t* rirb;             // Virtual address of RIRB entries.

    uint32_t corb_entry;        // Current CORB entry.
    uint32_t rirb_entry;        // Current RIRB entry.

    // Buffer refilling.
    uint32_t current_buffer_entry; // Current buffer entry for refilling.
    uint32_t buffer_entry_offset; // Buffers offset (how many times sound card went back to first buffer since playback start).

    uint32_t played_bytes; // Total amount of played bytes (out of sound_data_size).

    // Input/Recording support.
    uint64_t input_stream;        // Address of input stream descriptor.
    DMAAllocation input_buffer_entries_dma; // DMA allocation for input buffer entries.
    DMAAllocation input_buffers_dma;        // DMA allocation for input sound buffers.
    HdAudioBufferEntry* input_buffer_entries; // Input buffer entries array.
    uint8_t* input_data;          // Recorded audio data destination.
    uint32_t input_data_size;     // Size of recorded data.
    bool is_recording;            // Is currently recording?
    uint32_t recorded_bytes;      // Total amount of recorded bytes.
};

bool hda_is_initialized();

bool hda_is_paused();
bool hda_is_playing();

void hda_init();
void hda_reset();

void hda_set_volume(uint8_t volume);
uint8_t hda_get_volume();

void hda_set_channels(uint8_t channels);
void hda_set_bits_per_sample(uint8_t bits_per_sample);
void hda_set_sample_rate(uint32_t sample_rate);

void hda_play(uint8_t* data, uint32_t size);
void hda_resume();
void hda_pause();
void hda_stop();

void hda_poll();

uint32_t hda_get_played_bytes();
uint32_t hda_get_stream_position();

// Internal usage only.
uint16_t hda_get_node_connection_entry(HdAudioNode* node, uint32_t connection_entry_number);
void hda_power_on_node(HdAudioNode* node);

void hda_init_pin(HdAudioNode* node);
void hda_init_mixer(HdAudioNode* node);
void hda_init_output(HdAudioNode* node);

uint32_t hda_send_command(uint32_t codec, uint32_t node, uint32_t verb, uint32_t command);
void hda_set_node_volume(HdAudioNode* node, uint32_t volume);

// Per-channel volume control.
void hda_set_channel_volume(uint32_t node_id, uint8_t left, uint8_t right);

// Input/Recording support.
void hda_init_input_pin(HdAudioNode* node);
bool hda_is_recording();
void hda_record_start(uint8_t* buffer, uint32_t size);
void hda_record_stop();
void hda_record_poll();
uint32_t hda_get_recorded_bytes();

