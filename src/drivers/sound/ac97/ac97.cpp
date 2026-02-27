// AC'97 sound card driver for uniOS.
// Developed by Komok050505. 2025.

// Credits:
// - OSDev Wiki - Detailed information about AC'97 standard.
// - BleskOS AC'97 driver - Implementation example.
// - unionyxx - uniOS.

#include <drivers/sound/ac97/ac97.h>
#include <drivers/sound/wav.h>
#include <kernel/debug.h>
#include <kernel/fs/unifs.h>
#include <kernel/mm/pmm.h>
#include <kernel/mm/vmm.h>
#include <kernel/arch/x86_64/io.h>
#include <libk/kstring.h>
using kstring::memset;
using kstring::memcpy;

static Ac97Device ac97_info;

bool ac97_is_initialized() {
    return ac97_info.is_initialized;
}

bool ac97_is_paused() {
    return ac97_info.is_paused;
}

bool ac97_is_playing() {
    return ac97_info.is_playing;
}

// Initialize AC97 sound card.
void ac97_init() {
    if (ac97_info.is_initialized) return;

    PciDevice pci_dev;
    if (!pci_find_ac97(&pci_dev)) {
        return;
    }

    DEBUG_INFO("Found AC97 controller at %02x:%02x.%x", pci_dev.bus, pci_dev.device, pci_dev.function);

    pci_enable_io_space(&pci_dev);
    pci_enable_bus_mastering(&pci_dev);

    ac97_info.nam = pci_get_bar(&pci_dev, 0, nullptr);
    ac97_info.nabm = pci_get_bar(&pci_dev, 1, nullptr);

    outl(ac97_info.nabm + AC97_NABM_GLOBAL_CONTROL, (0b00<<AC97_NABM_GLOBAL_CONTROL_PCM_OUT_SAMPLES) | (0b00<<AC97_NABM_GLOBAL_CONTROL_PCM_OUT_CHANNELS) | (1<<AC97_NABM_GLOBAL_CONTROL_COLD_RESET));
    io_wait();

    outw(ac97_info.nam + AC97_NAM_RESET, 0x1);

    ac97_info.capabilities = inw(ac97_info.nam + AC97_NAM_EXTENDED_CAPABILITIES);
    if(ac97_info.capabilities & AC97_EXTENDED_CAPABILITY_VARIABLE_SAMPLE_RATE) {
        outw(ac97_info.nam + AC97_NAM_EXTENDED_FEATURES_CONTROL, AC97_EXTENDED_CAPABILITY_VARIABLE_SAMPLE_RATE);
        DEBUG_INFO("AC97: Variable sample rate supported");
    }

    outw(ac97_info.nam + AC97_NAM_PCM_OUT_VOLUME, 0x0);

    size_t buffer_entries_alloc_size = sizeof(Ac97BufferEntry) * AC97_BUFFER_ENTRY_COUNT;
    ac97_info.buffer_entries_dma = vmm_alloc_dma((buffer_entries_alloc_size + 4095) / 4096);
    ac97_info.buffer_entries = (Ac97BufferEntry*)ac97_info.buffer_entries_dma.virt;

    if (!ac97_info.buffer_entries_dma.virt) {
        DEBUG_ERROR("AC97: DMA allocation failed");
        return;
    }

    // Allocate memory for sound buffers.
    size_t sound_buffers_alloc_size = AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE * AC97_BUFFER_ENTRY_COUNT;
    ac97_info.sound_buffers_dma = vmm_alloc_dma((sound_buffers_alloc_size + 4095) / 4096);

    if (!ac97_info.sound_buffers_dma.virt || !ac97_info.sound_buffers_dma.phys) {
        DEBUG_ERROR("vmm_alloc_dma for sound buffers failed");
        return;
    }

    // Init complete.
    ac97_info.is_initialized = true;
    
    // Set default volume to 100%
    ac97_set_volume(100);

    DEBUG_INFO("init completed");
}

// Clean buffers and reset flags.
void ac97_reset() {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

    DEBUG_INFO("cleaning playback info");

    // Reset buffer info.
    ac97_info.current_buffer_entry = 0;
    ac97_info.buffer_entry_offset = 0;

    // Reset flags.
    ac97_info.is_playing = false;
    ac97_info.is_paused = false;

    ac97_info.played_bytes = 0;

    // Clean buffer entries and sound buffer.
    memset((void*)ac97_info.buffer_entries_dma.virt, 0, ac97_info.buffer_entries_dma.size);
    memset((void*)ac97_info.sound_buffers_dma.virt, 0, ac97_info.sound_buffers_dma.size);

    // Do not free this one! It's pointer to file system file data!
    ac97_info.sound_data = nullptr;
    ac97_info.sound_data_size = 0;
}

// Set master volume.
void ac97_set_volume(uint8_t volume) {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

    ac97_info.sound_volume = volume;

    if(volume == 0)
        outw(ac97_info.nam + AC97_NAM_MASTER_VOLUME, 0x8000);
    else {
        volume = ((100-volume)*AC97_NAM_PCM_OUT_VOLUME_STEPS/100);
        outw(ac97_info.nam + AC97_NAM_MASTER_VOLUME, ((volume) | (volume<<8)));
    }

    DEBUG_INFO("set volume to %d", ac97_info.sound_volume);
}

// Get master volume set previously.
uint8_t ac97_get_volume() {
    return ac97_info.sound_volume;
}

// Set channel count (placeholder - AC97 always uses stereo).
void ac97_set_channels(uint8_t channels) {
    (void)channels;
    // AC97 typically doesn't allow changing channels at runtime
    // Stereo (2 channels) is the default
}

// Set sample rate. Most common ones are 44100 and 48000 Hz.
void ac97_set_sample_rate(uint16_t sample_rate) {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

    // Set sample rate if supported by device.
    if (ac97_info.capabilities & AC97_EXTENDED_CAPABILITY_VARIABLE_SAMPLE_RATE) {
        outw(ac97_info.nam + AC97_NAM_VARIABLE_SAMPLE_RATE_FRONT_DAC, sample_rate);
        outw(ac97_info.nam + AC97_NAM_VARIABLE_SAMPLE_RATE_SURR_DAC, sample_rate);
        outw(ac97_info.nam + AC97_NAM_VARIABLE_SAMPLE_RATE_LFE_DAC, sample_rate);
        outw(ac97_info.nam + AC97_NAM_VARIABLE_SAMPLE_RATE_LR_ADC, sample_rate);
    }
}

// Play .wav audio file.
void ac97_play_wav_file(const char* filename) {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

    DEBUG_INFO("trying to play %s", filename);

    uint8_t* data_ptr;
    uint32_t data_size;
    uint32_t sample_rate;
    uint32_t channels;

    // Try to open WAV file.
    if (!wav_open(filename, &data_ptr, &data_size, &sample_rate, &channels)) {
        DEBUG_ERROR("wav_open failed");
        return;
    }

    // Set sample rate to one retrieved from WAV header.
    ac97_set_sample_rate(sample_rate);

    // Play it!
    ac97_play(data_ptr, data_size);
}

// Play raw .pcm audio file.
void ac97_play_pcm_file(const char* filename) {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

    DEBUG_INFO("trying to play %s", filename);

    UniFSFile file;
    if (!unifs_open_into(filename, &file)) {
        DEBUG_ERROR("unifs_open_into failed");
        return;
    }

    uint8_t* data_ptr = (uint8_t*)(uint64_t)file.data;
    uint32_t data_size = file.size;

    // FIXME: hard-coded sample rate value for ffmpeg .pcm files.
    ac97_set_sample_rate(22050);

    // Play it!
    ac97_play(data_ptr, data_size);
}

// Play PCM byte array.
void ac97_play(uint8_t* data, uint32_t size) {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

    // Do not play if sound card is already busy. In future we may add sound mixing.
    if (ac97_info.is_playing) {
        DEBUG_WARN("already playing! stop current playback before playing next sound");
        return;
    }

    // Reset stream.
    outb(ac97_info.nabm + AC97_NABM_PCM_OUT_CONTROL, AC97_NABM_PCM_OUT_CONTROL_RESET);

    DEBUG_INFO("waiting for reset");

    // Wait for reset.
    while (inb(ac97_info.nabm + AC97_NABM_PCM_OUT_CONTROL) & AC97_NABM_PCM_OUT_CONTROL_RESET) {
        io_wait();
    }

    // Clear status.
    outw(ac97_info.nabm + AC97_NABM_PCM_OUT_STATUS, 0x1C);

    DEBUG_INFO("playing sound data ptr: %p | data size: %d", data, size);

    // Set sound data source.
    ac97_info.sound_data = data;
    ac97_info.sound_data_size = size;

    // Copy new buffer data (bounded to avoid reading past source)
    size_t copy_size = (size < ac97_info.sound_buffers_dma.size) ? size : ac97_info.sound_buffers_dma.size;
    memcpy((void*)ac97_info.sound_buffers_dma.virt, data, copy_size);
    // Zero remaining buffer if source was smaller
    if (copy_size < ac97_info.sound_buffers_dma.size) {
        memset((void*)(ac97_info.sound_buffers_dma.virt + copy_size), 0, ac97_info.sound_buffers_dma.size - copy_size);
    }

    DEBUG_INFO("filling buffer entries");

    // Fill entries.
    uint64_t mem_offset = 0;
    for(uint32_t i = 0; i < AC97_BUFFER_ENTRY_COUNT; i++) {
        // Fill current entry.
        ac97_info.buffer_entries[i].buffer = (uint32_t)(ac97_info.sound_buffers_dma.phys + mem_offset);

        // Set sample count for buffer entry. AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE/2 because we use stereo sound data.
        ac97_info.buffer_entries[i].samples = AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE/2;

        // Add offset to read next sound data for next buffer entry. We don't want to listen to same buffer 24/7.
        mem_offset += AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE;
    }

    // Set buffer entry array base address.
    outl(ac97_info.nabm + AC97_NABM_PCM_OUT_BUFFER_BASE_ADDRESS, (uint32_t)ac97_info.buffer_entries_dma.phys);

    // Start stream.
    outb(ac97_info.nabm + AC97_NABM_PCM_OUT_CONTROL, AC97_NABM_PCM_OUT_CONTROL_START);

    DEBUG_INFO("started playback");

    // Let everyone know audio is playing.
    ac97_info.is_paused = false;
    ac97_info.is_playing = true;
}

// Resume playback if we played something before.
void ac97_resume() {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

    // Nothing to resume?
    if (!ac97_info.is_playing) {
        DEBUG_WARN("trying to resume playback, but nothing is played!");
        return;
    }

    // Start stream.
    outb(ac97_info.nabm + AC97_NABM_PCM_OUT_CONTROL, AC97_NABM_PCM_OUT_CONTROL_START);

    ac97_info.is_paused = false;
}

// Pause playback.
void ac97_pause() {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

    // Nothing to pause?
    if (!ac97_info.is_playing) {
        DEBUG_WARN("trying to pause playback, but nothing is played!");
        return;
    }

    // Stop stream.
    outb(ac97_info.nabm + AC97_NABM_PCM_OUT_CONTROL, AC97_NABM_PCM_OUT_CONTROL_STOP);

    ac97_info.is_paused = true;
}

// Full stop.
void ac97_stop() {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return;
    }

     // Nothing to stop?
    if (!ac97_info.is_playing) {
        DEBUG_WARN("trying to stop playback, but nothing is played!");
        return;
    }

    DEBUG_INFO("trying to reset stream");

    // Stop stream.
    outb(ac97_info.nabm + AC97_NABM_PCM_OUT_CONTROL, AC97_NABM_PCM_OUT_CONTROL_STOP);

    // Reset stream.
    outb(ac97_info.nabm + AC97_NABM_PCM_OUT_CONTROL, AC97_NABM_PCM_OUT_CONTROL_RESET);

    DEBUG_INFO("waiting for reset");

    // Wait for reset.
    while (inb(ac97_info.nabm + AC97_NABM_PCM_OUT_CONTROL) & AC97_NABM_PCM_OUT_CONTROL_RESET) {
        io_wait();
    }

    // Reset playback info.
    ac97_reset();

    DEBUG_INFO("stopped playback");
}

void ac97_poll() {
    // Check if initialized and playing.
    if (!ac97_info.is_initialized || !ac97_info.is_playing || ac97_info.is_paused) {
        return;
    }

    // Do not play further than source sound data.
    if (ac97_info.played_bytes >= ac97_info.sound_data_size) {
        ac97_stop();
        return;
    }

    // Get current stream position (between buffer entries) and buffer entry.
    uint32_t stream_pos = ac97_get_stream_position();
    uint32_t stream_curr_buffer_entry = inb(ac97_info.nabm + AC97_NABM_PCM_OUT_CURRENTLY_PROCESSED_ENTRY);

    // Update last valid buffer entry.
    outb(ac97_info.nabm + AC97_NABM_PCM_OUT_LAST_VALID_ENTRY, (stream_curr_buffer_entry - 1) & (AC97_BUFFER_ENTRY_COUNT - 1));

    // Well, this is not really good, but it works. If we use current buffer entry retrieved from sound card, sound may stutter.
    stream_curr_buffer_entry = stream_pos / AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE;

    // Reset current buffer entry and increase offset if sound card moved back to first entry.
    if (stream_curr_buffer_entry == 0 && ac97_info.current_buffer_entry > 0) {
        ac97_info.current_buffer_entry = 0;
        ac97_info.buffer_entry_offset++;

        // Calculate source offset and bounds-check before copying
        uint32_t src_offset = AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE * (AC97_BUFFER_ENTRY_COUNT * ac97_info.buffer_entry_offset + (AC97_BUFFER_ENTRY_COUNT - 1));
        void* dst = (void*)(ac97_info.sound_buffers_dma.virt + (AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE * (AC97_BUFFER_ENTRY_COUNT - 1)));
        if (src_offset < ac97_info.sound_data_size) {
            uint32_t avail = ac97_info.sound_data_size - src_offset;
            uint32_t copy_len = (avail < AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE) ? avail : AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE;
            memcpy(dst, ac97_info.sound_data + src_offset, copy_len);
            // Zero remainder if partial
            if (copy_len < AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE) {
                memset((void*)((uint8_t*)dst + copy_len), 0, AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE - copy_len);
            }
        } else {
            // Past end of data, zero the buffer
            memset(dst, 0, AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE);
        }
    }

    // Refill previous buffer with fresh data.
    if (stream_pos > (AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE * (ac97_info.current_buffer_entry + 1))) {
        // Calculate source offset and bounds-check before copying
        uint32_t src_offset = AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE * (AC97_BUFFER_ENTRY_COUNT * (ac97_info.buffer_entry_offset + 1) + ac97_info.current_buffer_entry);
        void* dst = (void*)(ac97_info.sound_buffers_dma.virt + (AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE * ac97_info.current_buffer_entry));
        if (src_offset < ac97_info.sound_data_size) {
            uint32_t avail = ac97_info.sound_data_size - src_offset;
            uint32_t copy_len = (avail < AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE) ? avail : AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE;
            memcpy(dst, ac97_info.sound_data + src_offset, copy_len);
            // Zero remainder if partial
            if (copy_len < AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE) {
                memset((void*)((uint8_t*)dst + copy_len), 0, AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE - copy_len);
            }
        } else {
            // Past end of data, zero the buffer
            memset(dst, 0, AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE);
        }

        // Now we can move to next entry.
        ac97_info.current_buffer_entry++;
    }

    ac97_info.played_bytes = (ac97_info.buffer_entry_offset * AC97_BUFFER_ENTRY_COUNT * AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE) + stream_pos;
}

uint32_t ac97_get_played_bytes() {
    return ac97_info.played_bytes;
}

// Get stream position between buffer entries.
uint32_t ac97_get_stream_position() {
    if (!ac97_info.is_initialized) {
        DEBUG_ERROR("ac97 device is not initialized");
        return 0;
    }

    uint8_t current_buffer = inb(ac97_info.nabm + AC97_NABM_PCM_OUT_CURRENTLY_PROCESSED_ENTRY);
    uint16_t buffer_offset = inw(ac97_info.nabm + AC97_NABM_PCM_OUT_CURRENT_ENTRY_POSITION);

    return current_buffer * AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE + (AC97_BUFFER_ENTRY_SOUND_BUFFER_SIZE/2 - buffer_offset);
}
