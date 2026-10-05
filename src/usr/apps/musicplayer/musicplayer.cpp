#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <uapi/event.h>
#include <uapi/sound.h>

#include "../../libapp/app.h"
#include "../../libapp/widgets.h"
#include "../../libc/log.h"
#include "../../libc/pthread.h"
#include "../../libc/unistd.h"
#include "../../libmedia/media_audio.h"

namespace {

enum PlayerPhase
{
    PH_EMPTY,   // no track ever loaded
    PH_PLAYING, // feeder running, card clocking out samples
    PH_PAUSED,  // feeder parked on the condvar, card paused
    PH_DONE,    // feeder reached EOF and drained
    PH_STOPPED, // stopped by the user (or displaced by another app)
    PH_ERROR    // load or stream failed
};

constexpr uint64_t PLAYER_SEEK_NONE = UINT64_MAX;
constexpr uint32_t FEEDER_CHUNK = 65536;
// Live scrubbing: while the seek slider is dragged, the position is applied
// at most this often (each apply restarts the kernel stream at the offset).
constexpr uint64_t SCRUB_INTERVAL_MS = 200;
// Build-time system asset (staged like the wallpaper), not user data.
constexpr const char *DEMO_TRACK = "/usr/share/music/demo.wav";

struct PlayerState
{
    // Feeder contract: phase/info/stream_base/seek_to are guarded by lock.
    pthread_mutex_t lock;
    pthread_cond_t cv;
    PlayerPhase phase;
    char path[256];
    media_audio_info info;
    char error_msg[128];
    uint64_t seek_to;      // absolute file offset, PLAYER_SEEK_NONE = none
    uint64_t stream_base;  // file offset of the first byte queued in the open stream
    uint64_t start_offset; // relative payload offset a fresh feeder starts from
    bool info_valid;       // feeder published info under lock
    bool feeder_alive;
    pthread_t feeder;

    // UI-only state below (touched by the draw/event thread alone).
    char open_path[256];
    bool initialized;
    bool card_present;
    bool seek_too_large_logged;
    uint64_t last_pos;           // payload bytes played, kept across DONE/STOPPED
    uint64_t last_seen_played;   // idle poll: stream played_bytes at last repaint
    PlayerPhase last_seen_phase; // idle poll: phase at last repaint
    uint64_t last_scrub_ticks;   // live scrub: last applied seek while dragging
    WidgetButton play;
    WidgetButton stop;
    WidgetButton demo;
    WidgetSlider seek;
    WidgetSlider volume;
};

const char *base_name(const char *path)
{
    const char *name = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            name = p + 1;
    }
    return name;
}

void format_time(char *out, size_t cap, uint64_t seconds)
{
    snprintf(out, cap, "%u:%02u", (unsigned)(seconds / 60), (unsigned)(seconds % 60));
}

uint64_t pcm_seconds(const media_audio_info *info, uint64_t payload_bytes)
{
    uint64_t frame = (uint64_t)info->sample_rate * info->channels * (info->bits_per_sample / 8);
    if (frame == 0)
        return 0;
    return payload_bytes / frame;
}

// The kernel stream is single and global: another app's stream_open resets
// ours. Ownership is observed through sound_status, never guessed from
// phases - only our own stream may be torn down or polled for position.
bool stream_owned()
{
    struct sound_status status;
    return sound_status(&status) == 0 && status.active && status.owned;
}

// Feeder-side helpers (the feeder owns stream_open/write/end; nothing else).

void feeder_set_error(PlayerState *st, const char *msg)
{
    pthread_mutex_lock(&st->lock);
    st->phase = PH_ERROR;
    snprintf(st->error_msg, sizeof(st->error_msg), "%s", msg);
    pthread_mutex_unlock(&st->lock);
    LOG_INFO("musicplayer", "error: %s", msg);
}

void *feeder_thread(void *arg)
{
    PlayerState *st = (PlayerState *)arg;

    int fd = open(st->path, O_RDONLY);
    if (fd < 0) {
        feeder_set_error(st, "open failed");
        return nullptr;
    }

    uint8_t *hdr = (uint8_t *)malloc(1024);
    uint8_t *chunk = (uint8_t *)malloc(FEEDER_CHUNK);
    if (!hdr || !chunk) {
        free(hdr);
        free(chunk);
        close(fd);
        feeder_set_error(st, "out of memory");
        return nullptr;
    }

    int64_t nread = read(fd, hdr, 1024);
    int64_t file_size = fsize(fd);
    media_audio_info local = {};
    if (nread <= 0 || file_size <= 0 || !media_audio_probe(hdr, (size_t)nread, (uint64_t)file_size, &local)) {
        free(hdr);
        free(chunk);
        close(fd);
        feeder_set_error(st, "not a playable wav");
        return nullptr;
    }

    pthread_mutex_lock(&st->lock);
    st->info = local;
    st->info_valid = true;
    uint64_t offset = local.data_start + st->start_offset;
    pthread_mutex_unlock(&st->lock);

    uint64_t payload_end = local.data_start + local.data_size;
    if (offset > payload_end)
        offset = payload_end;
    bool stream_mine = false;
    bool eof_signalled = false;

reopen:
    if (sound_stream_open(local.sample_rate, local.channels, local.bits_per_sample) != 0) {
        free(hdr);
        free(chunk);
        close(fd);
        feeder_set_error(st, "no audio device or bad format");
        return nullptr;
    }
    stream_mine = true;
    eof_signalled = false;
    lseek(fd, (int64_t)offset, SEEK_SET);
    pthread_mutex_lock(&st->lock);
    st->stream_base = offset;
    pthread_mutex_unlock(&st->lock);
    LOG_INFO("musicplayer", "play: %s (%u Hz, %u ch)", st->path, local.sample_rate, local.channels);

    for (;;) {
        pthread_mutex_lock(&st->lock);
        while (st->phase == PH_PAUSED && st->seek_to == PLAYER_SEEK_NONE)
            pthread_cond_wait(&st->cv, &st->lock);
        if (st->phase != PH_PLAYING && st->phase != PH_PAUSED) {
            pthread_mutex_unlock(&st->lock);
            goto out;
        }
        if (st->seek_to != PLAYER_SEEK_NONE) {
            uint64_t target = st->seek_to;
            st->seek_to = PLAYER_SEEK_NONE;
            pthread_mutex_unlock(&st->lock);
            if (target < local.data_start)
                target = local.data_start;
            if (target > payload_end)
                target = payload_end;
            offset = target;
            LOG_INFO("musicplayer", "seek: %u s", (unsigned)pcm_seconds(&local, target - local.data_start));
            // sound_stop is global (single kernel stream): only when the
            // stream is still ours may we tear it down for the restart.
            if (stream_mine)
                sound_stop();
            goto reopen;
        }
        pthread_mutex_unlock(&st->lock);

        if (offset >= payload_end) {
            if (!eof_signalled) {
                eof_signalled = true;
                if (stream_mine) {
                    sound_stream_end();
                    stream_mine = false;
                }
            }
            // The kernel ring holds more than most tracks: EOF of the reads
            // is not the end of the audio. Poll the drain inside the main
            // loop so the transport stays live (timer, pause, seek) until
            // the card has actually played everything - the kernel
            // auto-closes an ended, fully drained stream (and closes a
            // never-started empty one immediately), which is the
            // completion signal here.
            struct sound_status status;
            const bool have_status = sound_status(&status) == 0;
            const bool displaced = have_status && status.active && !status.owned;
            const bool drained = !have_status || !status.active;
            if (displaced || drained) {
                pthread_mutex_lock(&st->lock);
                // A seek that raced the drain completion must not be lost
                // to the terminal phase: re-check under the lock, and let
                // the loop top consume the pending seek instead.
                if (st->seek_to != PLAYER_SEEK_NONE) {
                    pthread_mutex_unlock(&st->lock);
                    continue;
                }
                st->phase = displaced ? PH_STOPPED : PH_DONE;
                pthread_mutex_unlock(&st->lock);
                LOG_INFO("musicplayer", displaced ? "stream taken over" : "done");
                goto out;
            }
            sleep_ms(40);
            continue;
        }

        uint64_t want = payload_end - offset;
        if (want > FEEDER_CHUNK)
            want = FEEDER_CHUNK;
        nread = read(fd, chunk, (size_t)want);
        if (nread <= 0) {
            feeder_set_error(st, "read failed");
            goto out;
        }

        // Streaming writes fail only when the stream was closed under us
        // (stop/seek restart, another opener, owner teardown): the write
        // syscall no longer has a legacy whole-buffer fallback.
        int64_t written = sound_write(chunk, (uint32_t)nread);
        if (written < 0) {
            pthread_mutex_lock(&st->lock);
            bool seek_pending = (st->seek_to != PLAYER_SEEK_NONE);
            // A seek carries its own restart; a bare loss demotes the
            // phase, or a respawned feeder would exit immediately.
            if (!seek_pending && (st->phase == PH_PLAYING || st->phase == PH_PAUSED))
                st->phase = PH_STOPPED;
            pthread_mutex_unlock(&st->lock);
            stream_mine = false;
            if (seek_pending)
                continue; // the seek branch above reopens the stream
            LOG_INFO("musicplayer", "stream stopped");
            goto out;
        }
        offset += (uint64_t)nread;
    }

out:
    if (stream_mine)
        sound_stop();
    free(hdr);
    free(chunk);
    close(fd);
    return nullptr;
}

// UI-side actions (the UI thread owns pause/resume/stop/volume/status).

// Byte offsets handed to the player must land on whole PCM sample frames:
// the kernel stream rejects partial-frame writes, so a slider position or
// restart offset is floored to the frame size before use.
uint64_t align_to_frame(const media_audio_info *info, uint64_t rel_bytes)
{
    const uint64_t frame = (uint64_t)info->channels * (info->bits_per_sample / 8);
    if (frame == 0)
        return rel_bytes;
    return rel_bytes - rel_bytes % frame;
}

// Every action decides under one lock hold at action time - a phase
// snapshot taken at event entry could be stale by the time the click fires.

void player_start(PlayerState *st, uint64_t start_offset)
{
    if (!st->path[0] || !st->card_present)
        return;

    // Reap a feeder that exited on its own (DONE/ERROR); stop one that is
    // still running. The phase flip plus signal also unparks a feeder
    // condvar-waiting in PAUSED, or the join hangs; a write-blocked one
    // observes the closed stream instead. Only our own stream may be
    // stopped - after DONE/ERROR another app may already be playing.
    if (st->feeder_alive) {
        pthread_mutex_lock(&st->lock);
        st->phase = PH_STOPPED;
        st->seek_to = PLAYER_SEEK_NONE;
        pthread_mutex_unlock(&st->lock);
        if (stream_owned())
            sound_stop();
        pthread_cond_signal(&st->cv);
        pthread_join(st->feeder, nullptr);
        st->feeder_alive = false;
    }

    pthread_mutex_lock(&st->lock);
    st->phase = PH_PLAYING;
    st->seek_to = PLAYER_SEEK_NONE;
    start_offset = align_to_frame(&st->info, start_offset);
    st->start_offset = start_offset;
    st->info_valid = false;
    st->error_msg[0] = '\0';
    pthread_mutex_unlock(&st->lock);
    st->last_pos = start_offset; // UI-only: the new track starts here
    st->last_seen_played = ~(uint64_t)0;

    if (pthread_create(&st->feeder, nullptr, feeder_thread, st) == 0) {
        st->feeder_alive = true;
    } else {
        pthread_mutex_lock(&st->lock);
        st->phase = PH_ERROR;
        snprintf(st->error_msg, sizeof(st->error_msg), "thread create failed");
        pthread_mutex_unlock(&st->lock);
        LOG_INFO("musicplayer", "error: thread create failed");
    }
}

void player_toggle_play(PlayerState *st)
{
    if (!st->path[0] || !st->card_present)
        return;

    enum ToggleAction
    {
        TOGGLE_PAUSE,
        TOGGLE_RESUME,
        TOGGLE_START,
    } action;

    pthread_mutex_lock(&st->lock);
    if (st->phase == PH_PLAYING) {
        st->phase = PH_PAUSED;
        action = TOGGLE_PAUSE;
    } else if (st->phase == PH_PAUSED) {
        st->phase = PH_PLAYING;
        action = TOGGLE_RESUME;
    } else {
        // EMPTY/DONE/STOPPED/ERROR: (re)start from the beginning.
        action = TOGGLE_START;
    }
    pthread_mutex_unlock(&st->lock);

    if (action == TOGGLE_PAUSE) {
        sound_pause();
        LOG_INFO("musicplayer", "pause");
    } else if (action == TOGGLE_RESUME) {
        sound_resume();
        pthread_cond_signal(&st->cv);
        LOG_INFO("musicplayer", "resume");
    } else {
        player_start(st, 0);
    }
}

void player_stop(PlayerState *st)
{
    pthread_mutex_lock(&st->lock);
    const bool live = (st->phase == PH_PLAYING || st->phase == PH_PAUSED);
    st->phase = PH_STOPPED;
    st->seek_to = PLAYER_SEEK_NONE;
    pthread_mutex_unlock(&st->lock);

    if (st->feeder_alive) {
        // Unpark a feeder waiting in PAUSED; one blocked in sound_write
        // wakes on the closed stream and exits through the same phase
        // check. A foreign stream (another app took the card) is not ours
        // to stop.
        if (live && stream_owned())
            sound_stop();
        pthread_cond_signal(&st->cv);
        pthread_join(st->feeder, nullptr);
        st->feeder_alive = false;
    }
    // A stopped player returns to the start of the track.
    st->last_pos = 0;
    st->last_seen_played = ~(uint64_t)0;
    LOG_INFO("musicplayer", "stop");
}

void player_seek(PlayerState *st, uint64_t rel_bytes)
{
    pthread_mutex_lock(&st->lock);
    if (!st->info_valid) {
        pthread_mutex_unlock(&st->lock);
        return;
    }
    rel_bytes = align_to_frame(&st->info, rel_bytes);
    if (rel_bytes > st->info.data_size)
        rel_bytes = st->info.data_size;
    st->seek_to = st->info.data_start + rel_bytes;
    const bool feeder_live = st->feeder_alive && (st->phase == PH_PLAYING || st->phase == PH_PAUSED);
    pthread_mutex_unlock(&st->lock);

    if (!feeder_live) {
        // Finished, stopped or failed: a seek restarts playback at the
        // offset instead of being a dead control.
        player_start(st, rel_bytes);
        return;
    }

    // Break a write blocked on a full ring; the feeder re-reads the state
    // after the failed write and takes the seek branch. Only our own
    // stream may be torn down for the restart.
    if (stream_owned())
        sound_stop();
    pthread_cond_signal(&st->cv);
    // Hold the target during the restart so the position display does not
    // snap back to the pre-seek spot while the ring refills.
    st->last_pos = rel_bytes;
}

void player_teardown(PlayerState *st)
{
    if (!st->feeder_alive)
        return;
    pthread_mutex_lock(&st->lock);
    st->phase = PH_STOPPED;
    pthread_mutex_unlock(&st->lock);
    if (stream_owned())
        sound_stop();
    pthread_cond_signal(&st->cv);
    pthread_join(st->feeder, nullptr);
    st->feeder_alive = false;
}

bool transport_available(const PlayerState *st)
{
    return st->card_present && st->path[0];
}

// Drawing ---------------------------------------------------------------------

void draw_centered_text(Surface *s, const GuiFont *font, int y, const char *text, uint32_t fg)
{
    int text_w = gui_measure_text(font, text);
    int x = (static_cast<int>(s->width) - text_w) / 2;
    if (x < 0)
        x = 0;
    gui_draw_text_clipped(s, font, x, y, static_cast<int>(s->width), text, fg, g_gui_style.app_bg);
}

struct PlayerSnapshot
{
    PlayerPhase phase;
    media_audio_info info;
    bool info_valid;
    char error_msg[128];
};

PlayerSnapshot take_snapshot(PlayerState *st)
{
    PlayerSnapshot snap;
    pthread_mutex_lock(&st->lock);
    snap.phase = st->phase;
    snap.info_valid = st->info_valid;
    if (snap.info_valid)
        snap.info = st->info;
    else
        memset(&snap.info, 0, sizeof(snap.info));
    snprintf(snap.error_msg, sizeof(snap.error_msg), "%s", st->error_msg);
    pthread_mutex_unlock(&st->lock);
    return snap;
}

void musicplayer_draw(App *app, Surface *canvas)
{
    PlayerState *st = (PlayerState *)app_user(app);

    if (!st->initialized) {
        st->initialized = true;
        struct sound_status boot_status;
        st->card_present = (sound_status(&boot_status) == 0) && boot_status.card_present;
        if (!st->card_present)
            LOG_INFO("musicplayer", "no audio device detected");
        if (st->open_path[0]) {
            LOG_INFO("musicplayer", "open request: %s", st->open_path);
            strncpy(st->path, st->open_path, sizeof(st->path) - 1);
            st->path[sizeof(st->path) - 1] = '\0';
            player_start(st, 0);
        } else {
            LOG_INFO("musicplayer", "ready");
        }
    }

    gui_fill_surface(canvas, g_gui_style.app_bg);
    int header_h = gui_headerbar_h();
    int view_w = static_cast<int>(canvas->width);
    int view_h = static_cast<int>(canvas->height) - header_h;
    int view_x = 0;

    if (!st->path[0]) {
        int title_h = gui_font_line_height(gui_font_title());
        int line_h = gui_line_height();
        int control_h = gui_app_control_h();
        int rows = title_h + gui_space_1() + line_h * 2 + (st->card_present ? gui_space_2() + control_h : 0);
        int y = header_h + (view_h - rows) / 2;
        if (y < header_h)
            y = header_h;
        draw_centered_text(canvas, gui_font_title(), y, "No track open", g_gui_style.text);
        y += title_h + gui_space_1();
        draw_centered_text(canvas, gui_font_default(), y, "Open audio from Files, or play the demo.",
                           g_gui_style.text_muted);
        y += line_h * 2 + (st->card_present ? gui_space_2() : 0);
        if (st->card_present) {
            // The demo track is a system asset; playing it must not depend
            // on Files exposing a system path next to the user's Home.
            int demo_w = gui_scaled_metric(120);
            st->demo.rect = gui_rect_make((view_w - demo_w) / 2, y, demo_w, control_h);
            widget_button_draw(canvas, &st->demo, "Play Demo", true, true);
        }
        return;
    }

    PlayerSnapshot snap = take_snapshot(st);

    // Poll the stream position (transport syscalls are UI-owned). Only a
    // stream that is still ours is a valid position source: a foreign
    // stream (another app opened one) resets ours, and its numbers would
    // be garbage here. The last position is kept so DONE/STOPPED keep
    // showing where playback ended, and a seek holds its target through
    // the restart.
    uint64_t played_rel = st->last_pos;
    bool stream_clocking = false;
    if (snap.phase == PH_PLAYING || snap.phase == PH_PAUSED) {
        struct sound_status status;
        if (sound_status(&status) == 0 && status.active && status.owned && snap.info_valid) {
            uint64_t base;
            pthread_mutex_lock(&st->lock);
            base = st->stream_base;
            pthread_mutex_unlock(&st->lock);
            if (base >= snap.info.data_start)
                played_rel = base + status.played_bytes - snap.info.data_start;
            st->last_pos = played_rel;
            stream_clocking = status.playing != 0;
        }
    }
    if (snap.info_valid && played_rel > snap.info.data_size)
        played_rel = snap.info.data_size;

    // Column layout, centered in the view area. Sliders use the height the
    // slider widget is drawn for (label row + track); button-height rects
    // make the label, percent and knob overlap.
    int title_h = gui_font_line_height(gui_font_title());
    int line_h = gui_line_height();
    int control_h = gui_app_control_h();
    int control_row = control_h + gui_space_1() * 2;
    int slider_row = gui_app_slider_h() + gui_space_1() * 2;
    int gap = gui_space_2();
    int column_w = view_w < gui_scaled_metric(420) ? view_w - gui_scaled_metric(32) : gui_scaled_metric(380);
    if (column_w < gui_scaled_metric(200))
        column_w = gui_scaled_metric(200);

    // A track is only seekable while its payload fits the slider's uint32;
    // oversized tracks hide the slider and log once.
    bool seek_usable = snap.info_valid && snap.info.data_size > 0 && snap.info.data_size <= 0xFFFFFFFFu;
    if (snap.info_valid && snap.info.data_size > 0xFFFFFFFFu && !st->seek_too_large_logged) {
        st->seek_too_large_logged = true;
        LOG_INFO("musicplayer", "seek disabled: track too large");
    }

    int rows = title_h + gui_space_1() + line_h + gap;
    if (st->card_present)
        rows += (seek_usable ? slider_row : 0) + control_row + slider_row + line_h;
    int y = header_h + (view_h - rows) / 2;
    if (y < header_h)
        y = header_h;

    const GuiFont *title_font = gui_font_title();
    int name_w = gui_measure_text(title_font, base_name(st->path));
    int name_x = (view_w - name_w) / 2;
    if (name_x < view_x)
        name_x = view_x;
    gui_draw_text_clipped(canvas, title_font, name_x, y, view_w, base_name(st->path), g_gui_style.text,
                          g_gui_style.app_bg);
    y += title_h + gui_space_1();

    // Format line (or the no-card notice: without a card there is no
    // transport to control, so the layout stops here).
    char meta_line[64];
    if (!st->card_present) {
        snprintf(meta_line, sizeof(meta_line), "No audio device detected");
    } else if (snap.info_valid) {
        snprintf(meta_line, sizeof(meta_line), "%u Hz, %s, 16-bit", snap.info.sample_rate,
                 snap.info.channels == 1 ? "mono" : "stereo");
    } else {
        meta_line[0] = '\0';
    }
    if (meta_line[0]) {
        draw_centered_text(canvas, gui_font_default(), y, meta_line, g_gui_style.text_muted);
    }
    y += line_h + gap;

    if (!st->card_present)
        return; // no transport, nothing to control

    // Seek slider with the position as its readout. While dragging, the
    // readout follows the dragged position (scrub preview), not the stream.
    if (seek_usable) {
        st->seek.rect = gui_rect_make(view_x + (view_w - column_w) / 2, y, column_w, gui_app_slider_h());
        if (!st->seek.dragging)
            st->seek.value = (uint32_t)played_rel;
        char cur[16], total[16], time_text[40];
        uint64_t shown_rel = st->seek.dragging ? st->seek.value : played_rel;
        format_time(cur, sizeof(cur), pcm_seconds(&snap.info, shown_rel));
        format_time(total, sizeof(total), pcm_seconds(&snap.info, snap.info.data_size));
        snprintf(time_text, sizeof(time_text), "%s / %s", cur, total);
        widget_slider_draw_ex(canvas, &st->seek, "Seek", (uint32_t)snap.info.data_size, time_text);
        y += slider_row;
    }

    // Transport row: Play/Pause + Stop, equal-width so the pair reads as
    // one control cluster.
    int play_w = gui_scaled_metric(88);
    int stop_w = gui_scaled_metric(88);
    int row_w = play_w + gui_space_1() + stop_w;
    int row_x = view_x + (view_w - row_w) / 2;
    st->play.rect = gui_rect_make(row_x, y, play_w, control_h);
    st->stop.rect = gui_rect_make(row_x + play_w + gui_space_1(), y, stop_w, control_h);
    bool active = transport_available(st) && snap.phase != PH_EMPTY;
    widget_button_draw(canvas, &st->play, (snap.phase == PH_PLAYING) ? "Pause" : "Play", true, active);
    widget_button_draw(canvas, &st->stop, "Stop", false, active);
    y += control_row + gui_space_2();

    // Volume slider: system registry value, card register writes only.
    st->volume.rect = gui_rect_make(view_x + (view_w - column_w) / 2, y, column_w, gui_app_slider_h());
    widget_slider_draw(canvas, &st->volume, "Volume", 100);
    y += slider_row + gui_space_1();

    // Status line: transport state or the load/stream error.
    char status_line[96];
    uint32_t status_fg = g_gui_style.text_muted;
    if (snap.error_msg[0]) {
        snprintf(status_line, sizeof(status_line), "%s", snap.error_msg);
        status_fg = g_gui_style.text;
    } else {
        switch (snap.phase) {
            case PH_PLAYING:
                if (!snap.info_valid)
                    snprintf(status_line, sizeof(status_line), "Opening...");
                else if (!stream_clocking)
                    snprintf(status_line, sizeof(status_line), "Buffering...");
                else
                    snprintf(status_line, sizeof(status_line), "Playing");
                break;
            case PH_PAUSED:
                snprintf(status_line, sizeof(status_line), "Paused");
                break;
            case PH_DONE:
                snprintf(status_line, sizeof(status_line), "Finished");
                break;
            case PH_STOPPED:
                snprintf(status_line, sizeof(status_line), "Stopped");
                break;
            default:
                status_line[0] = '\0';
                break;
        }
    }
    if (status_line[0])
        draw_centered_text(canvas, gui_font_default(), y, status_line, status_fg);
}

void musicplayer_idle(App *app)
{
    // The app draws only on invalidation, so playback progress would freeze
    // on the last input-driven frame. Repaint when the stream position
    // advances (or the feeder flips the phase with no input involved).
    PlayerState *st = (PlayerState *)app_user(app);
    if (!st->card_present)
        return;
    PlayerSnapshot snap = take_snapshot(st);
    if (snap.phase != st->last_seen_phase) {
        st->last_seen_phase = snap.phase;
        app_invalidate_all(app);
    }
    if (snap.phase != PH_PLAYING)
        return;
    struct sound_status status;
    if (sound_status(&status) != 0 || !status.active || !status.owned)
        return;
    if (status.played_bytes != st->last_seen_played) {
        st->last_seen_played = status.played_bytes;
        app_invalidate_all(app);
    }
}

void musicplayer_event(App *app, const Event *ev)
{
    PlayerState *st = (PlayerState *)app_user(app);
    PlayerSnapshot snap = take_snapshot(st);
    if (!transport_available(st))
        snap.phase = PH_EMPTY; // no path or card: widgets stay inert
    bool seek_enabled = snap.info_valid && snap.info.data_size > 0 && snap.info.data_size <= 0xFFFFFFFFu;

    switch (ev->type) {
        case EVT_MOUSE_MOVE: {
            // Slider drags are global: keep tracking after the pointer leaves
            // the slider rect until release. Otherwise feed the move for
            // hover tracking only.
            if (st->seek.dragging) {
                if (seek_enabled) {
                    widget_slider_event(&st->seek, ev, (uint32_t)snap.info.data_size);
                    // Live scrub: apply the dragged position while
                    // dragging, throttled - every apply restarts the
                    // kernel stream at the offset, and the release applies
                    // the final position.
                    if (get_ticks() - st->last_scrub_ticks >= SCRUB_INTERVAL_MS) {
                        st->last_scrub_ticks = get_ticks();
                        player_seek(st, st->seek.value);
                    }
                }
                app_invalidate_all(app);
            } else if (st->volume.dragging) {
                if (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED)
                    sound_volume(st->volume.value);
                app_invalidate_all(app);
            } else {
                bool changed = false;
                if (seek_enabled)
                    changed |=
                        (widget_slider_event(&st->seek, ev, (uint32_t)snap.info.data_size) & WIDGET_CHANGED) != 0;
                changed |= (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED) != 0;
                changed |= (widget_button_event(&st->play, ev) & WIDGET_CHANGED) != 0;
                changed |= (widget_button_event(&st->stop, ev) & WIDGET_CHANGED) != 0;
                if (!st->path[0])
                    changed |= (widget_button_event(&st->demo, ev) & WIDGET_CHANGED) != 0;
                if (changed)
                    app_invalidate_all(app);
            }
            break;
        }

        case EVT_MOUSE_DOWN: {
            if (ev->mouse.button != 1)
                break;
            if (st->seek.dragging || st->volume.dragging)
                break;
            bool changed = false;
            if (seek_enabled)
                changed |= (widget_slider_event(&st->seek, ev, (uint32_t)snap.info.data_size) & WIDGET_CHANGED) != 0;
            changed |= (widget_slider_event(&st->volume, ev, 100) & WIDGET_CHANGED) != 0;
            // Buttons track the press here so release-to-apply can fire on UP.
            changed |= (widget_button_event(&st->play, ev) & WIDGET_CHANGED) != 0;
            changed |= (widget_button_event(&st->stop, ev) & WIDGET_CHANGED) != 0;
            if (!st->path[0])
                changed |= (widget_button_event(&st->demo, ev) & WIDGET_CHANGED) != 0;
            if (changed)
                app_invalidate_all(app);
            break;
        }

        case EVT_MOUSE_UP: {
            if (ev->mouse.button != 1)
                break;
            int seek_ev = 0, vol_ev = 0;
            if (seek_enabled)
                seek_ev = widget_slider_event(&st->seek, ev, (uint32_t)snap.info.data_size);
            vol_ev = widget_slider_event(&st->volume, ev, 100);
            if ((seek_ev & WIDGET_CHANGED) || (vol_ev & WIDGET_CHANGED))
                app_invalidate_all(app);

            if (vol_ev & WIDGET_CLICKED)
                sound_volume(st->volume.value);

            if ((seek_ev & WIDGET_CLICKED) && seek_enabled) {
                // Slider max is the payload size, so value is already the
                // relative byte offset. Dead phases restart at the offset.
                player_seek(st, st->seek.value);
                app_invalidate_all(app);
            }

            if (widget_button_event(&st->play, ev) & WIDGET_CLICKED) {
                player_toggle_play(st);
                app_invalidate_all(app);
            }

            // The demo button exists only in the empty state: its rect
            // would otherwise stay stale after a track loads and silently
            // restart the demo on clicks in that area.
            if (!st->path[0] && widget_button_event(&st->demo, ev) & WIDGET_CLICKED) {
                strncpy(st->path, DEMO_TRACK, sizeof(st->path) - 1);
                st->path[sizeof(st->path) - 1] = '\0';
                player_start(st, 0);
                app_invalidate_all(app);
            }

            if (widget_button_event(&st->stop, ev) & WIDGET_CLICKED) {
                player_stop(st);
                app_invalidate_all(app);
            }
            break;
        }

        case EVT_UNFOCUS:
        case EVT_MOUSE_LEAVE:
            // A drag that ended outside the window (the WM takes over the
            // pointer) would otherwise leave dragging stuck and swallow
            // every later MOUSE_DOWN (preferences.cpp does the same).
            widget_button_reset(&st->play);
            widget_button_reset(&st->stop);
            widget_button_reset(&st->demo);
            widget_slider_reset(&st->seek);
            widget_slider_reset(&st->volume);
            app_invalidate_all(app);
            break;

        default:
            break;
    }
}

void musicplayer_menus(App *app)
{
    (void)app;
    MenuModel model;
    gui_menu_model_reset(&model);
    app_menus_add_help(&model, 0);
    gui_menu_publish(&model);
}

} // namespace

extern "C" int main()
{
    static PlayerState st = {};
    pthread_mutex_init(&st.lock, nullptr);
    pthread_cond_init(&st.cv, nullptr);
    st.seek_to = PLAYER_SEEK_NONE;

    if (gui_open_request_take(st.open_path, sizeof(st.open_path)))
        st.open_path[sizeof(st.open_path) - 1] = '\0';
    else
        st.open_path[0] = '\0';

    // Mirror the system volume into the slider; writes happen on user input.
    Registry *registry = gui_registry();
    st.volume.value = registry ? registry->volume_level : 100;
    if (st.volume.value > 100)
        st.volume.value = 100;

    AppConfig config = {};
    config.title = "Music";
    config.width = gui_scaled_metric(520);
    config.height = gui_scaled_metric(420);
    config.min_width = gui_scaled_metric(320);
    config.min_height = gui_scaled_metric(240);
    config.flags = WIN_FLAG_RESIZABLE;
    config.idle_ms = 33;
    config.on_draw = musicplayer_draw;
    config.on_event = musicplayer_event;
    config.on_idle = musicplayer_idle;
    config.on_menus = musicplayer_menus;

    int code = app_run(&config, &st);
    player_teardown(&st);
    return code;
}
