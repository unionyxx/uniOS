# Audio

Audio playback supports AC97 and High Definition Audio controllers (`src/drivers/sound/`). `sound_init()` initializes both, prefers HDA, and allocates a 2 MiB streaming ring; all operations dispatch to the active card. Playback is polled — neither driver uses interrupts — and `sound_poll()` is pumped from the kernel idle loop, which is what keeps both whole-file and streaming playback fed.

## Common Model

- Stream configuration: sample rate, channels, bits.
- File playback by extension (`SYS_SOUND_PLAY`): `.wav` (PCM-only parser, RIFF chunk scan), `.mp3` (parser stub — not implemented), otherwise raw PCM at 22050 Hz stereo.
- Buffer descriptor lists: 32 entries of 32 KB each in both drivers. Legacy whole-file playback copies up to one ring of the source up front; the poll function refills consumed entries from the source buffer as the hardware plays them.
- Volume: `sound_set_volume(0-100)` programs the card's master volume (NAM master volume on AC97, output widget gain on HDA).

## Streaming Playback

Userspace decodes audio and pushes PCM instead of buffering whole files (`src/drivers/sound/sound.cpp`):

| Call | Behavior |
| --- | --- |
| `SYS_SOUND_STREAM_OPEN(rate, channels, 16)` | Stops any current playback, configures the card, opens the stream. 16-bit only; 1 or 2 channels. |
| `SYS_SOUND_WRITE(data, size)` | With a stream open, blocks until every byte is queued in the ring (`-EPIPE` if the stream was stopped, `-EINTR` on fatal signal). Without a stream open, legacy whole-buffer playback. |
| `SYS_SOUND_STREAM_END` | No more data; drain what is queued, then auto-close. |
| `SYS_SOUND_STOP` / `SYS_SOUND_PAUSE` / `SYS_SOUND_RESUME` | Transport control; pause/resume are idempotent. |
| `SYS_SOUND_STATUS` | Fills `sound_status` (`include/uapi/sound.h`): played/queued bytes in source format, state flags. |
| `SYS_SOUND_VOLUME(level)` | Card master volume, 0-100. |

Mechanics:

- The dispatcher ring is 2 MiB. DMA starts once a full card ring (1 MiB = 32 x 32 KiB) is queued, so playback never begins with silence; `STREAM_START_THRESHOLD` gates this. A stream end with less queued flushes immediately.
- The drivers run in `stream_mode`: BDL refills pull from the ring through a refill callback instead of a fixed source buffer. An exhausted ring pads silence and the card stops when the last real byte is consumed; the dispatcher restarts the card when enough new data is queued (underrun recovery) or closes the stream after a drained `STREAM_END`.
- AC97 DMAs stereo pairs, so mono streams are upmixed to stereo in the dispatcher; HDA passes the source channel count through.
- Writers block on a wait queue while the ring is full and are woken by the refill path, so playback never depends on holding a whole decoded file in memory. Blocking writers rely on the idle-loop pump advancing the card.

## AC97

`src/drivers/sound/ac97/ac97.cpp` (PCI class 0x04 / subclass 0x01):

- NAM (BAR0) and NABM (BAR1) I/O registers; cold reset through the NABM global control.
- Variable sample rate detection via the extended capabilities register.
- PCM-out stream: BDL base, last-valid index, start/stop/reset controls. Volume via the NAM master volume register (inverted attenuation).
- Playback only.

## HDA

`src/drivers/sound/hda/hda.cpp` (PCI class 0x04 / subclass 0x03), MMIO BAR0:

- Controller reset, STATESTS codec scan, CORB/RIRB command rings (256 entries each) for codec verbs, plus an immediate-command path for QEMU.
- Codec/widget tree discovery: AFG nodes, pin/mixer/output widgets, EAPD, amplifier gain setup.
- Playback and recording streams (output stream chosen after input streams); per-node channel volume.

## Userspace Path

| Command / call | Route |
| --- | --- |
| `sound <file>` (shell) | `SYS_SOUND_PLAY` — kernel-side parser picks WAV/MP3/PCM |
| `play <file>` (shell) | Userspace WAV parser, then `SYS_SOUND_CONFIG` + `SYS_SOUND_WRITE` raw PCM |
| libgui/app code | `sound_config(rate, channels, 16)` then `sound_write(data, size)` |
