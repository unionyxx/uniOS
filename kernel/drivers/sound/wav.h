#pragma once
#include <stdint.h>
#include <stdbool.h>

struct __attribute__((packed)) WavRiffDescriptor {
    uint8_t         riff[4];        // RIFF Header Magic header
    uint32_t        chunk_size;     // RIFF Chunk Size (file size - 8)
    uint8_t         wave[4];        // WAVE Header
};

struct __attribute__((packed)) WavFmtChunk {
    uint8_t         fmt[4];         // FMT header
    uint32_t        chunk_size;    // Size of the fmt chunk
    uint16_t        audio_format;   // Audio format 1=PCM,6=mulaw,7=alaw,257=IBM Mu-Law, 258=IBM A-Law, 259=ADPCM
    uint16_t        channels;       // Number of channels 1=Mono 2=Stereo
    uint32_t        samples;        // Sampling Frequency in Hz
    uint32_t        bytes_per_second;  // bytes per second
    uint16_t        block_align;    // 2=16-bit mono, 4=16-bit stereo
    uint16_t        bits_per_sample;   // Number of bits per sample
};

struct __attribute__((packed)) WavDataChunk {
    uint8_t         data[4];
    uint32_t        data_size;

    uint8_t         data_;
};

struct WavHeader {
    WavRiffDescriptor* riff_descriptor;
    WavFmtChunk* fmt_chunk;
    WavDataChunk* data_chunk;
};

bool wav_open(const char* filename, uint8_t** data, uint32_t* data_size, uint32_t* sample_rate, uint32_t* channels);
