#include <drivers/sound/sound.h>

#include <drivers/sound/ac97/ac97.h>
#include <drivers/sound/hda/hda.h>

#include <drivers/sound/wav.h>
#include <drivers/sound/mp3.h>

#include <kernel/fs/unifs.h>
#include <kernel/debug.h>

static bool sound_available = false;

static bool ac97_available = false;
static bool hda_available = false;

static uint8_t preferred_sound_card = SOUND_HD_AUDIO;
static uint8_t used_sound_card = SOUND_NONE;

bool sound_is_initialized() {
    return sound_available;
}

uint8_t sound_get_card_type() {
    return used_sound_card;
}

bool sound_is_paused() {
    return used_sound_card == SOUND_HD_AUDIO ? hda_is_paused() : ac97_is_paused();
}

bool sound_is_playing() {
    return used_sound_card == SOUND_HD_AUDIO ? hda_is_playing() : ac97_is_playing();
}

void sound_init() {
    // Initialize AC97 sound driver
    ac97_init();

    // Initialize HD Audio sound driver
    hda_init();

    // Which ones are ready? HD Audio is always preferred.
    ac97_available = ac97_is_initialized();
    hda_available = hda_is_initialized();

    // Is any sound card available?
    sound_available = ac97_available || hda_available;

    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    // Determine which sound card to use based on preferred one and available ones.
    if (preferred_sound_card == SOUND_AC97 && ac97_available) {
        used_sound_card = SOUND_AC97;
    }
    else if (preferred_sound_card == SOUND_HD_AUDIO && hda_available) {
        used_sound_card = SOUND_HD_AUDIO;
    }
    else {
        used_sound_card = hda_available ? SOUND_HD_AUDIO : SOUND_AC97;
    }
}

void sound_reset() {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_reset() : ac97_reset();
}

void sound_set_volume(uint8_t volume) {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_set_volume(volume) : ac97_set_volume(volume);
}

uint8_t sound_get_volume() {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return 0;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_get_volume() : ac97_get_volume();
}

void sound_set_channels(uint8_t channels) {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_set_channels(channels) : ac97_set_channels(channels);
}

void sound_set_bits_per_sample(uint8_t bits_per_sample) {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    if (used_sound_card == SOUND_HD_AUDIO) {
        return hda_set_bits_per_sample(bits_per_sample);
    }

    DEBUG_WARN("not available on ac97");
}

void sound_set_sample_rate(uint32_t sample_rate) {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_set_sample_rate(sample_rate) : ac97_set_sample_rate(sample_rate);
}

void sound_play_mp3_file(const char* filename) {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    DEBUG_INFO("trying to play %s", filename);

    uint8_t* data_ptr;
    uint32_t data_size;

    uint32_t sample_rate;
    uint32_t channels;

    // Try to open MP3 file.
    if (!mp3_open(filename, &data_ptr, &data_size, &sample_rate, &channels)) {
        DEBUG_ERROR("mp3_open failed");
        return;
    }

    // 16-bit only for now.
    sound_set_bits_per_sample(16);

    // Set channel count to one retrieved from decoder.
    sound_set_channels(channels);

    // Set sample rate to one retrieved from decoder.
    sound_set_sample_rate(sample_rate);

    // Play it!
    sound_play(data_ptr, data_size);
}

void sound_play_wav_file(const char* filename) {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
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

    // 16-bit only for now.
    sound_set_bits_per_sample(16);

    // Set channel count to one retrieved from WAV header.
    sound_set_channels(channels);

    // Set sample rate to one retrieved from WAV header.
    sound_set_sample_rate(sample_rate);

    // Play it!
    sound_play(data_ptr, data_size);
}

void sound_play_pcm_file(const char* filename) {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
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

    // 16-bit only for now.
    sound_set_bits_per_sample(16);

    // FIXME: hard-coded sample rate and channel count values for ffmpeg .pcm files.
    sound_set_channels(2);
    sound_set_sample_rate(22050);

    // Play it!
    sound_play(data_ptr, data_size);
}

void sound_play(uint8_t* data, uint32_t size) {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_play(data, size) : ac97_play(data, size);
}

void sound_resume() {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_resume() : ac97_resume();
}

void sound_pause() {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_pause() : ac97_pause();
}

void sound_stop() {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_stop() : ac97_stop();
}

void sound_poll() {
    if (!sound_available) {
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_poll() : ac97_poll();
}

uint32_t sound_get_played_bytes() {
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return 0;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_get_played_bytes() : ac97_get_played_bytes();
}
