#include <drivers/sound/ac97/ac97.h>
#include <drivers/sound/hda/hda.h>
#include <drivers/sound/mp3.h>
#include <drivers/sound/sound.h>
#include <drivers/sound/wav.h>
#include <kernel/debug.h>
#include <kernel/fs/vfs.h>
#include <kernel/mm/heap.h>
#include <kernel/process.h>
#include <kernel/scheduler.h>
#include <kernel/sync/spinlock.h>
#include <kernel/syscall.h>
#include <libk/kstring.h>
#include <uapi/sound.h>

static bool sound_available = false;

static bool ac97_available = false;
static bool hda_available = false;

static uint8_t preferred_sound_card = SOUND_HD_AUDIO;
static uint8_t used_sound_card = SOUND_NONE;

bool sound_is_initialized()
{
    return sound_available;
}

uint8_t sound_get_card_type()
{
    return used_sound_card;
}

bool sound_is_paused()
{
    return used_sound_card == SOUND_HD_AUDIO ? hda_is_paused() : ac97_is_paused();
}

bool sound_is_playing()
{
    return used_sound_card == SOUND_HD_AUDIO ? hda_is_playing() : ac97_is_playing();
}

void sound_init()
{
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
        DEBUG_INFO("No compatible sound card detected; audio disabled");
        return;
    }

    // Determine which sound card to use based on preferred one and available ones.
    if (preferred_sound_card == SOUND_AC97 && ac97_available) {
        used_sound_card = SOUND_AC97;
    } else if (preferred_sound_card == SOUND_HD_AUDIO && hda_available) {
        used_sound_card = SOUND_HD_AUDIO;
    } else {
        used_sound_card = hda_available ? SOUND_HD_AUDIO : SOUND_AC97;
    }

    sound_stream_ring_init();

    DEBUG_SUCCESS("Sound system active using %s", used_sound_card == SOUND_HD_AUDIO ? "Intel HD Audio" : "AC97");
}

void sound_reset()
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_reset() : ac97_reset();
}

void sound_set_volume(uint8_t volume)
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_set_volume(volume) : ac97_set_volume(volume);
}

uint8_t sound_get_volume()
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return 0;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_get_volume() : ac97_get_volume();
}

void sound_set_channels(uint8_t channels)
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_set_channels(channels) : ac97_set_channels(channels);
}

void sound_set_bits_per_sample(uint8_t bits_per_sample)
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    if (used_sound_card == SOUND_HD_AUDIO) {
        return hda_set_bits_per_sample(bits_per_sample);
    }

    DEBUG_WARN("not available on ac97");
}

void sound_set_sample_rate(uint32_t sample_rate)
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_set_sample_rate(sample_rate)
                                             : ac97_set_sample_rate((uint16_t)sample_rate);
}

void sound_play_mp3_file(const char *filename)
{
    if (!sound_available) {
        DEBUG_ERROR("Sound system not available");
        return;
    }

    uint8_t *data_ptr;
    uint32_t data_size;
    uint32_t sample_rate;
    uint32_t channels;

    if (!mp3_open(filename, &data_ptr, &data_size, &sample_rate, &channels)) {
        DEBUG_ERROR("Failed to open MP3 file: %s", filename);
        return;
    }

    DEBUG_INFO("Playing MP3: %s (%lu Hz, %d channels, %lu bytes)", filename, sample_rate, channels, data_size);

    sound_set_bits_per_sample(16);
    sound_set_channels((uint8_t)channels);
    sound_set_sample_rate(sample_rate);
    sound_play(data_ptr, data_size);
}

void sound_play_wav_file(const char *filename)
{
    if (!sound_available) {
        DEBUG_ERROR("Sound system not available");
        return;
    }

    uint8_t *data_ptr;
    uint32_t data_size;
    uint32_t sample_rate;
    uint32_t channels;
    uint8_t *buffer_ptr;

    if (!wav_open(filename, &data_ptr, &data_size, &sample_rate, &channels, &buffer_ptr)) {
        DEBUG_ERROR("Failed to open WAV file: %s", filename);
        return;
    }

    DEBUG_INFO("Playing WAV: %s (%lu Hz, %d channels, %lu bytes)", filename, sample_rate, channels, data_size);

    sound_set_bits_per_sample(16);
    sound_set_channels((uint8_t)channels);
    sound_set_sample_rate(sample_rate);
    sound_play(buffer_ptr, data_size);
}

void sound_play_pcm_file(const char *filename)
{
    if (!sound_available) {
        DEBUG_ERROR("Sound system not available");
        return;
    }

    VNodeStat st;
    if (vfs_stat(filename, &st) < 0) {
        DEBUG_ERROR("Failed to stat PCM file: %s", filename);
        return;
    }

    int fd = vfs_open(filename, O_RDONLY);
    if (fd < 0) {
        DEBUG_ERROR("Failed to open PCM file: %s", filename);
        return;
    }

    uint8_t *data_ptr = (uint8_t *)malloc(st.size);
    if (!data_ptr) {
        vfs_close(fd);
        DEBUG_ERROR("Out of memory for PCM file");
        return;
    }

    int64_t bytes_read = vfs_read(fd, data_ptr, st.size);
    vfs_close(fd);

    if (bytes_read < (int64_t)st.size) {
        free(data_ptr);
        DEBUG_ERROR("Failed to read PCM file");
        return;
    }

    DEBUG_INFO("Playing raw PCM: %s (%lu bytes)", filename, st.size);

    sound_set_bits_per_sample(16);
    sound_set_channels(2);
    sound_set_sample_rate(22050);
    sound_play(data_ptr, (uint32_t)st.size);
}

void sound_play(uint8_t *data, uint32_t size)
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_play(data, size) : ac97_play(data, size);
}

void sound_resume()
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_resume() : ac97_resume();
}

void sound_pause()
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_pause() : ac97_pause();
}

void sound_stop()
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_stop() : ac97_stop();
}

// ---------------------------------------------------------------------------
// Streaming playback
//
// Userspace decodes audio and pushes PCM through SYS_SOUND_WRITE. The
// dispatcher buffers it in a kernel ring, starts DMA once a full card ring is
// queued, and feeds the card's BDL refills from the ring until the stream is
// ended or stopped. Writers block while the ring is full, so playback never
// requires holding a whole decoded file in memory.
// ---------------------------------------------------------------------------

namespace {

constexpr uint32_t STREAM_RING_SIZE = 2u * 1024u * 1024u;
// Both cards use 32 BDL entries of 32 KiB = 1 MiB. DMA only starts once the
// ring can pre-fill the whole card buffer, so playback never begins with
// silence padding.
constexpr uint32_t STREAM_START_THRESHOLD = 32u * 0x8000u;
constexpr uint32_t STREAM_MAX_WRITE = 4u * 1024u * 1024u;

struct SoundStreamState
{
    Spinlock lock;
    WaitQueue space_wait;
    bool open;        // stream accepted writes
    bool ended;       // writer promised no more data; drain then auto-close
    bool dma_started; // card is clocking data out of the ring
    bool start_requested;
    uint8_t *ring; // allocated once at boot, kept for the system lifetime
    uint32_t read_pos;
    uint32_t write_pos;
    uint32_t count;
    uint32_t rate;
    uint8_t src_channels;        // format the writer supplies
    uint8_t dst_channels;        // what the card DMAs (AC97 upmixes mono to stereo)
    uint64_t played_card_bytes;  // consumed by the card, card-format bytes
    uint32_t last_driver_played; // driver counter at previous poll
};

SoundStreamState g_stream = {SPINLOCK_INIT, {nullptr, nullptr}};

inline uint32_t stream_src_frame()
{
    return (uint32_t)g_stream.src_channels * 2u;
}

inline uint32_t stream_dst_frame()
{
    return (uint32_t)g_stream.dst_channels * 2u;
}

// Ring write of `len` already-formatted bytes from `src`. Lock held.
void stream_push_bytes(const uint8_t *src, uint32_t len)
{
    uint32_t tail = STREAM_RING_SIZE - g_stream.write_pos;
    uint32_t first = tail < len ? tail : len;
    kstring::memcpy(g_stream.ring + g_stream.write_pos, src, first);
    if (len > first)
        kstring::memcpy(g_stream.ring, src + first, len - first);
    g_stream.write_pos = (g_stream.write_pos + len) % STREAM_RING_SIZE;
    g_stream.count += len;
}

// BDL refill callback handed to the card drivers. Runs from sound_poll().
uint32_t stream_refill(uint8_t *dst, uint32_t len)
{
    uint64_t flags = spinlock_acquire_irqsave(&g_stream.lock);
    if (!g_stream.open || !g_stream.ring || g_stream.count == 0) {
        spinlock_release_irqrestore(&g_stream.lock, flags);
        return 0;
    }
    uint32_t take = g_stream.count < len ? g_stream.count : len;
    uint32_t tail = STREAM_RING_SIZE - g_stream.read_pos;
    uint32_t first = tail < take ? tail : take;
    kstring::memcpy(dst, g_stream.ring + g_stream.read_pos, first);
    if (take > first)
        kstring::memcpy(dst + first, g_stream.ring, take - first);
    g_stream.read_pos = (g_stream.read_pos + take) % STREAM_RING_SIZE;
    g_stream.count -= take;
    spinlock_release_irqrestore(&g_stream.lock, flags);
    // Consuming data frees ring space; unblock writers.
    scheduler_wake_all(&g_stream.space_wait);
    return take;
}

// Lock held. Does not release.
void stream_reset_locked()
{
    g_stream.open = false;
    g_stream.ended = false;
    g_stream.dma_started = false;
    g_stream.start_requested = false;
    g_stream.read_pos = 0;
    g_stream.write_pos = 0;
    g_stream.count = 0;
    g_stream.played_card_bytes = 0;
    g_stream.last_driver_played = 0;
}

} // namespace

void sound_stream_ring_init()
{
    if (g_stream.ring)
        return;
    g_stream.ring = static_cast<uint8_t *>(malloc(STREAM_RING_SIZE));
    if (!g_stream.ring)
        DEBUG_WARN("sound: failed to allocate stream ring; streaming playback disabled");
}

bool sound_stream_open(uint32_t sample_rate, uint32_t channels, uint32_t bits_per_sample)
{
    if (!sound_available || !g_stream.ring)
        return false;
    if (bits_per_sample != 16 || (channels != 1 && channels != 2) || sample_rate == 0 || sample_rate > 192000)
        return false;

    uint64_t flags = spinlock_acquire_irqsave(&g_stream.lock);
    bool was_open = g_stream.open;
    stream_reset_locked();
    spinlock_release_irqrestore(&g_stream.lock, flags);

    // A fresh stream takes over the card: stop any previous stream or legacy
    // whole-buffer playback first.
    if (was_open || sound_is_playing())
        sound_stop();

    const uint8_t dst_channels = used_sound_card == SOUND_AC97 ? 2 : static_cast<uint8_t>(channels);
    sound_set_sample_rate(sample_rate);
    sound_set_channels(dst_channels);
    sound_set_bits_per_sample(16);

    flags = spinlock_acquire_irqsave(&g_stream.lock);
    g_stream.open = true;
    g_stream.rate = sample_rate;
    g_stream.src_channels = static_cast<uint8_t>(channels);
    g_stream.dst_channels = dst_channels;
    spinlock_release_irqrestore(&g_stream.lock, flags);
    return true;
}

int64_t sound_stream_write(const void *data, uint32_t len)
{
    if (!data || len == 0 || len > STREAM_MAX_WRITE)
        return -22; // -EINVAL

    uint64_t flags = spinlock_acquire_irqsave(&g_stream.lock);
    if (!g_stream.open || !g_stream.ring) {
        spinlock_release_irqrestore(&g_stream.lock, flags);
        return -32; // -EPIPE: no stream open
    }

    const uint32_t src_frame = stream_src_frame();
    const uint32_t dst_frame = stream_dst_frame();
    if (len % src_frame != 0) {
        spinlock_release_irqrestore(&g_stream.lock, flags);
        return -22; // -EINVAL: partial sample frame
    }
    const uint64_t dst_bytes = (uint64_t)len / src_frame * dst_frame;

    const uint8_t *src = static_cast<const uint8_t *>(data);
    uint64_t produced = 0;
    while (produced < dst_bytes) {
        if (!g_stream.open) {
            spinlock_release_irqrestore(&g_stream.lock, flags);
            return -32; // -EPIPE: stream stopped while writing
        }
        if (scheduler_fatal_signal_pending(process_get_current())) {
            spinlock_release_irqrestore(&g_stream.lock, flags);
            return -4; // -EINTR: do not sleep through a fatal signal
        }

        uint32_t space = STREAM_RING_SIZE - g_stream.count;
        if (space == 0) {
            scheduler_wait(&g_stream.space_wait, &g_stream.lock);
            continue;
        }

        uint64_t remain = dst_bytes - produced;
        uint32_t chunk = remain < space ? static_cast<uint32_t>(remain) : space;
        chunk -= chunk % dst_frame;

        if (src_frame == dst_frame) {
            stream_push_bytes(src + produced, chunk);
        } else {
            // Mono -> stereo upmix for cards that DMA stereo pairs.
            const int16_t *samples = reinterpret_cast<const int16_t *>(src + produced);
            uint32_t frames = chunk / dst_frame;
            for (uint32_t i = 0; i < frames; i++) {
                uint8_t pair[4];
                pair[0] = static_cast<uint8_t>(samples[i] & 0xFF);
                pair[1] = static_cast<uint8_t>((samples[i] >> 8) & 0xFF);
                pair[2] = pair[0];
                pair[3] = pair[1];
                stream_push_bytes(pair, sizeof(pair));
            }
        }
        produced += chunk;
    }

    spinlock_release_irqrestore(&g_stream.lock, flags);
    return static_cast<int64_t>(len);
}

void sound_stream_end()
{
    uint64_t flags = spinlock_acquire_irqsave(&g_stream.lock);
    if (g_stream.open)
        g_stream.ended = true;
    spinlock_release_irqrestore(&g_stream.lock, flags);
}

void sound_stream_stop()
{
    uint64_t flags = spinlock_acquire_irqsave(&g_stream.lock);
    bool was_open = g_stream.open;
    stream_reset_locked();
    spinlock_release_irqrestore(&g_stream.lock, flags);

    if (was_open && sound_is_playing())
        sound_stop();
    // Blocked writers must observe the closed stream instead of sleeping on.
    scheduler_wake_all(&g_stream.space_wait);
}

bool sound_stream_active()
{
    uint64_t flags = spinlock_acquire_irqsave(&g_stream.lock);
    bool active = g_stream.open;
    spinlock_release_irqrestore(&g_stream.lock, flags);
    return active;
}

bool sound_stream_status(struct sound_status *out)
{
    if (!out)
        return false;

    uint64_t flags = spinlock_acquire_irqsave(&g_stream.lock);
    const bool open = g_stream.open;
    const bool dma_started = g_stream.dma_started;
    const bool ended = g_stream.ended;
    const uint32_t src_frame = stream_src_frame();
    const uint32_t dst_frame = stream_dst_frame();
    const uint64_t played_card = g_stream.played_card_bytes;
    const uint64_t queued_card = g_stream.count;
    out->sample_rate = g_stream.rate;
    out->channels = g_stream.src_channels;
    out->bits_per_sample = 16;
    spinlock_release_irqrestore(&g_stream.lock, flags);

    out->played_bytes = dst_frame ? played_card * src_frame / dst_frame : 0;
    out->queued_bytes = dst_frame ? queued_card * src_frame / dst_frame : 0;
    out->active = open ? 1 : 0;
    out->playing = (open && dma_started && !ended && sound_is_playing() && !sound_is_paused()) ? 1 : 0;
    out->paused = (open && dma_started && sound_is_paused()) ? 1 : 0;
    out->card_present = sound_available ? 1 : 0;
    out->reserved[0] = out->reserved[1] = out->reserved[2] = 0;
    return true;
}

void sound_poll()
{
    if (!sound_available) {
        return;
    }

    // Only one core pumps the card at a time (SMP idle loops).
    static volatile uint32_t pump_busy = 0;
    if (__sync_lock_test_and_set(&pump_busy, 1u))
        return;

    uint64_t flags = spinlock_acquire_irqsave(&g_stream.lock);
    const bool stream_open = g_stream.open;
    spinlock_release_irqrestore(&g_stream.lock, flags);

    if (!stream_open) {
        // Legacy whole-buffer playback pump.
        if (used_sound_card == SOUND_HD_AUDIO)
            hda_poll();
        else
            ac97_poll();
        __sync_lock_release(&pump_busy);
        return;
    }

    const bool was_playing = sound_is_playing();
    if (used_sound_card == SOUND_HD_AUDIO)
        hda_poll();
    else
        ac97_poll();

    bool start_now = false;
    flags = spinlock_acquire_irqsave(&g_stream.lock);
    if (g_stream.open) {
        const bool now_playing = sound_is_playing();
        if (g_stream.dma_started) {
            // Accumulate consumed bytes as deltas so driver-side resets on
            // stop never rewind the stream's progress counter.
            uint32_t drv_played = sound_get_played_bytes();
            if (drv_played >= g_stream.last_driver_played)
                g_stream.played_card_bytes += drv_played - g_stream.last_driver_played;
            g_stream.last_driver_played = now_playing ? drv_played : 0;

            if (was_playing && !now_playing) {
                g_stream.dma_started = false;
                if (g_stream.ended) {
                    // Fully drained: auto-close so waiters/players move on.
                    stream_reset_locked();
                    spinlock_release_irqrestore(&g_stream.lock, flags);
                    scheduler_wake_all(&g_stream.space_wait);
                    __sync_lock_release(&pump_busy);
                    return;
                }
            }
        }
        if (!g_stream.dma_started && !g_stream.start_requested &&
            (g_stream.count >= STREAM_START_THRESHOLD || (g_stream.ended && g_stream.count > 0))) {
            g_stream.start_requested = true;
            start_now = true;
        }
    }
    spinlock_release_irqrestore(&g_stream.lock, flags);

    if (start_now) {
        // Program the card outside the stream lock: reset sequences busy-wait.
        if (used_sound_card == SOUND_HD_AUDIO)
            hda_stream_start(stream_refill);
        else
            ac97_stream_start(stream_refill);

        flags = spinlock_acquire_irqsave(&g_stream.lock);
        g_stream.start_requested = false;
        if (g_stream.open) {
            g_stream.dma_started = true;
            g_stream.last_driver_played = 0;
        }
        spinlock_release_irqrestore(&g_stream.lock, flags);
    }

    __sync_lock_release(&pump_busy);
}

void sound_fill_dma_buffer(uint8_t *dst, uint8_t *src, uint32_t src_size, uint32_t offset, uint32_t chunk_size)
{
    if (offset < src_size) {
        uint32_t avail = src_size - offset;
        uint32_t copy_len = (avail < chunk_size) ? avail : chunk_size;
        kstring::memcpy(dst, src + offset, copy_len);
        if (copy_len < chunk_size) {
            kstring::memset(dst + copy_len, 0, chunk_size - copy_len);
        }
    } else {
        kstring::memset(dst, 0, chunk_size);
    }
}

uint32_t sound_get_played_bytes()
{
    if (!sound_available) {
        DEBUG_ERROR("sound card not found. audio is not available");
        return 0;
    }

    return used_sound_card == SOUND_HD_AUDIO ? hda_get_played_bytes() : ac97_get_played_bytes();
}
