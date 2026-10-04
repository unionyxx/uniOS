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
    PH_STOPPED, // stopped by the user
    PH_ERROR    // load or stream failed
};

constexpr uint64_t PLAYER_SEEK_NONE = UINT64_MAX;
constexpr uint32_t FEEDER_CHUNK = 65536;

struct PlayerState
{
    // Feeder contract: phase/info/stream_base/seek_to are guarded by lock.
    pthread_mutex_t lock;
    pthread_cond_t cv;
    PlayerPhase phase;
    char path[256];
    media_audio_info info;
    char error_msg[128];
    uint64_t seek_to;     // absolute file offset, PLAYER_SEEK_NONE = none
    uint64_t stream_base; // file offset of the first byte queued in the open stream
    bool info_valid;      // feeder published info under lock
    bool feeder_alive;
    pthread_t feeder;

    // UI-only state below (touched by the draw/event thread alone).
    char open_path[256];
    bool initialized;
    bool card_present;
    bool seek_too_large_logged;
    uint64_t last_pos; // payload bytes played, kept across DONE/STOPPED
    WidgetButton play;
    WidgetButton stop;
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
    pthread_mutex_unlock(&st->lock);

    uint64_t offset = local.data_start;
    uint64_t payload_end = local.data_start + local.data_size;
    bool stream_mine = false;
    bool first_open = true;

reopen:
    if (sound_stream_open(local.sample_rate, local.channels, local.bits_per_sample) != 0) {
        free(hdr);
        free(chunk);
        close(fd);
        feeder_set_error(st, "no audio device or bad format");
        return nullptr;
    }
    stream_mine = true;
    lseek(fd, (int64_t)offset, SEEK_SET);
    pthread_mutex_lock(&st->lock);
    st->stream_base = offset;
    pthread_mutex_unlock(&st->lock);
    if (first_open) {
        first_open = false;
        LOG_INFO("musicplayer", "play: %s (%u Hz, %u ch)", st->path, local.sample_rate, local.channels);
    }

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
            if (target < local.data_start)
                target = local.data_start;
            if (target > payload_end)
                target = payload_end;
            offset = target;
            st->seek_to = PLAYER_SEEK_NONE;
            pthread_mutex_unlock(&st->lock);
            LOG_INFO("musicplayer", "seek: %u s", (unsigned)pcm_seconds(&local, target - local.data_start));
            // sound_stop is global (single kernel stream): only when the
            // stream is still ours may we tear it down for the restart.
            if (stream_mine) {
                sound_stop();
                stream_mine = false;
            }
            goto reopen;
        }
        pthread_mutex_unlock(&st->lock);

        if (offset >= payload_end) {
            if (stream_mine) {
                sound_stream_end();
                stream_mine = false;
            }
            pthread_mutex_lock(&st->lock);
            st->phase = PH_DONE;
            pthread_mutex_unlock(&st->lock);
            LOG_INFO("musicplayer", "done");
            goto out;
        }

        uint64_t want = payload_end - offset;
        if (want > FEEDER_CHUNK)
            want = FEEDER_CHUNK;
        nread = read(fd, chunk, (size_t)want);
        if (nread <= 0) {
            feeder_set_error(st, "read failed");
            goto out;
        }

        // Classic syscalls mask every negative return to -1 (there is no
        // errno channel), so any failure here means the stream was stopped
        // under us: the kernel's own EPIPE/EINTR distinction is unobservable.
        int64_t written = sound_write(chunk, (uint32_t)nread);
        if (written < 0) {
            pthread_mutex_lock(&st->lock);
            bool seek_pending = (st->seek_to != PLAYER_SEEK_NONE);
            // A seek carries its own restart; only a bare stop demotes the
            // phase, or the reopened feeder would exit immediately.
            if (!seek_pending && st->phase == PH_PLAYING)
                st->phase = PH_STOPPED;
            pthread_mutex_unlock(&st->lock);
            stream_mine = false;
            if (seek_pending)
                continue; // the seek branch above reopens the stream
            // UI stop or teardown - or another app took the single stream.
            // Either way this feeder must not write another byte: without a
            // stream sound_write falls into the legacy whole-buffer path.
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

void player_start(PlayerState *st)
{
    if (!st->path[0] || !st->card_present)
        return;

    // Reap a feeder that exited on its own (DONE/ERROR); stop one that is
    // somehow still running before respawning. The phase flip plus signal
    // also unparks a feeder condvar-waiting in PAUSED, or the join hangs.
    if (st->feeder_alive) {
        pthread_mutex_lock(&st->lock);
        PlayerPhase live = st->phase;
        st->phase = PH_STOPPED;
        st->seek_to = PLAYER_SEEK_NONE;
        pthread_mutex_unlock(&st->lock);
        // Only touch the global stream when the dying feeder could still
        // own it; on DONE/ERROR another app may already be playing.
        if (live == PH_PLAYING || live == PH_PAUSED)
            sound_stop();
        pthread_cond_signal(&st->cv);
        pthread_join(st->feeder, nullptr);
        st->feeder_alive = false;
    }

    pthread_mutex_lock(&st->lock);
    st->phase = PH_PLAYING;
    st->seek_to = PLAYER_SEEK_NONE;
    st->info_valid = false;
    st->error_msg[0] = '\0';
    pthread_mutex_unlock(&st->lock);
    st->last_pos = 0; // UI-only: the new track starts at zero

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

void player_pause(PlayerState *st)
{
    pthread_mutex_lock(&st->lock);
    st->phase = PH_PAUSED;
    pthread_mutex_unlock(&st->lock);
    sound_pause();
    LOG_INFO("musicplayer", "pause");
}

void player_resume(PlayerState *st)
{
    pthread_mutex_lock(&st->lock);
    st->phase = PH_PLAYING;
    pthread_mutex_unlock(&st->lock);
    sound_resume();
    pthread_cond_signal(&st->cv);
    LOG_INFO("musicplayer", "resume");
}

void player_stop(PlayerState *st)
{
    pthread_mutex_lock(&st->lock);
    st->phase = PH_STOPPED;
    pthread_mutex_unlock(&st->lock);
    sound_stop();
    // Unpark a feeder waiting in PAUSED; one blocked in sound_write wakes on
    // -EPIPE and exits through the same phase check.
    pthread_cond_signal(&st->cv);
    if (st->feeder_alive) {
        pthread_join(st->feeder, nullptr);
        st->feeder_alive = false;
    }
    LOG_INFO("musicplayer", "stop");
}

void player_seek(PlayerState *st, uint64_t rel_bytes)
{
    pthread_mutex_lock(&st->lock);
    if (!st->info_valid) {
        pthread_mutex_unlock(&st->lock);
        return;
    }
    st->seek_to = st->info.data_start + rel_bytes;
    pthread_mutex_unlock(&st->lock);
    // Break a write blocked on a full ring; the feeder re-reads the state on
    // -EPIPE and takes the seek branch. Also stops the stream for the
    // restart the feeder performs there.
    sound_stop();
    pthread_cond_signal(&st->cv);
}

void player_teardown(PlayerState *st)
{
    if (!st->feeder_alive)
        return;
    pthread_mutex_lock(&st->lock);
    st->phase = PH_STOPPED;
    pthread_mutex_unlock(&st->lock);
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
            player_start(st);
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
        int y = header_h + (view_h - gui_font_line_height(gui_font_title()) - gui_line_height() * 2) / 2;
        if (y < header_h)
            y = header_h;
        draw_centered_text(canvas, gui_font_title(), y, "No track open", g_gui_style.text);
        draw_centered_text(canvas, gui_font_default(), y + gui_font_line_height(gui_font_title()) + gui_space_1(),
                           "Open audio from Files, or play the demo from /Music.", g_gui_style.text_muted);
        return;
    }

    PlayerSnapshot snap = take_snapshot(st);

    // Poll the stream position (transport syscalls are UI-owned; the status
    // struct is a kernel-side snapshot and needs no lock). The last position
    // is kept so DONE/STOPPED keep showing where playback ended; the clamp
    // also bounds a stale last_pos until the new track's first poll lands.
    uint64_t played_rel = st->last_pos;
    if (snap.phase == PH_PLAYING || snap.phase == PH_PAUSED) {
        struct sound_status status;
        if (sound_status(&status) == 0 && status.active && snap.info_valid) {
            uint64_t base;
            pthread_mutex_lock(&st->lock);
            base = st->stream_base;
            pthread_mutex_unlock(&st->lock);
            if (base >= snap.info.data_start)
                played_rel = base + status.played_bytes - snap.info.data_start;
            st->last_pos = played_rel;
        }
    }
    if (snap.info_valid && played_rel > snap.info.data_size)
        played_rel = snap.info.data_size;

    // Column layout, centered in the view area.
    int title_h = gui_font_line_height(gui_font_title());
    int line_h = gui_line_height();
    int control_h = gui_app_control_h();
    int control_row = gui_app_control_h() + gui_space_1() * 2;
    int gap = gui_space_2();
    int column_w = view_w < gui_scaled_metric(420) ? view_w - gui_scaled_metric(32) : gui_scaled_metric(380);
    if (column_w < gui_scaled_metric(200))
        column_w = gui_scaled_metric(200);

    bool seek_usable = snap.info_valid && snap.info.data_size > 0 && snap.info.data_size <= 0xFFFFFFFFu;
    if (snap.info_valid && snap.info.data_size > 0xFFFFFFFFu && !st->seek_too_large_logged) {
        st->seek_too_large_logged = true;
        LOG_INFO("musicplayer", "seek disabled: track too large");
    }

    int rows = title_h + line_h + control_row + control_row + (snap.error_msg[0] ? line_h : 0);
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

    char time_line[96];
    if (!st->card_present) {
        snprintf(time_line, sizeof(time_line), "No audio device detected");
    } else if (snap.info_valid) {
        char cur[16], total[16];
        uint64_t played_sec = pcm_seconds(&snap.info, played_rel);
        format_time(cur, sizeof(cur), played_sec);
        format_time(total, sizeof(total), pcm_seconds(&snap.info, snap.info.data_size));
        if (snap.phase == PH_PAUSED)
            snprintf(time_line, sizeof(time_line), "%s / %s   Paused", cur, total);
        else
            snprintf(time_line, sizeof(time_line), "%s / %s", cur, total);
    } else if (snap.error_msg[0]) {
        snprintf(time_line, sizeof(time_line), "%s", snap.error_msg);
    } else {
        snprintf(time_line, sizeof(time_line), "Loading...");
    }
    draw_centered_text(canvas, gui_font_default(), y, time_line,
                       snap.error_msg[0] ? g_gui_style.text : g_gui_style.text_muted);
    y += line_h + gap;
    if (snap.error_msg[0]) {
        // Reserved row: surface mid-play errors (read failed) that the time
        // row cannot show once info is valid.
        draw_centered_text(canvas, gui_font_default(), y, snap.error_msg, g_gui_style.text);
        y += line_h + gui_space_1();
    }

    // Transport row: Play/Pause + Stop.
    int play_w = gui_scaled_metric(96);
    int stop_w = gui_scaled_metric(80);
    int row_w = play_w + gui_space_1() + stop_w;
    int row_x = view_x + (view_w - row_w) / 2;
    st->play.rect = gui_rect_make(row_x, y, play_w, control_h);
    st->stop.rect = gui_rect_make(row_x + play_w + gui_space_1(), y, stop_w, control_h);
    bool active = transport_available(st) && snap.phase != PH_EMPTY;
    widget_button_draw(canvas, &st->play, (snap.phase == PH_PLAYING) ? "Pause" : "Play", true, active);
    widget_button_draw(canvas, &st->stop, "Stop", false, active);
    y += control_row + gui_space_2();

    // Seek slider: value in payload bytes, live-tracked unless dragging.
    st->seek.rect = gui_rect_make(view_x + (view_w - column_w) / 2, y, column_w, control_h);
    if (!st->seek.dragging) {
        if (seek_usable)
            st->seek.value = (uint32_t)played_rel;
        else
            st->seek.value = 0;
    }
    widget_slider_draw(canvas, &st->seek, seek_usable ? "Seek" : "Seek (disabled)",
                       seek_usable ? (uint32_t)snap.info.data_size : 100);
    y += control_row + gui_space_2();

    // Volume slider: system registry value, card register writes only.
    st->volume.rect = gui_rect_make(view_x + (view_w - column_w) / 2, y, column_w, control_h);
    widget_slider_draw(canvas, &st->volume, "Volume", 100);
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
                if (seek_enabled)
                    widget_slider_event(&st->seek, ev, (uint32_t)snap.info.data_size);
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

            if ((seek_ev & WIDGET_CLICKED) && (snap.phase == PH_PLAYING || snap.phase == PH_PAUSED)) {
                // Slider max is the payload size, so value is already the
                // relative byte offset.
                player_seek(st, st->seek.value);
                app_invalidate_all(app);
            }

            if (widget_button_event(&st->play, ev) & WIDGET_CLICKED) {
                if (snap.phase == PH_PLAYING) {
                    player_pause(st);
                } else if (snap.phase == PH_PAUSED) {
                    player_resume(st);
                } else {
                    player_start(st);
                }
                app_invalidate_all(app);
            }

            if (widget_button_event(&st->stop, ev) & WIDGET_CLICKED) {
                if (snap.phase == PH_PLAYING || snap.phase == PH_PAUSED) {
                    player_stop(st);
                    app_invalidate_all(app);
                }
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
    config.on_menus = musicplayer_menus;

    int code = app_run(&config, &st);
    player_teardown(&st);
    return code;
}
