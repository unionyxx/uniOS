# Audio

Audio playback supports AC97 and High Definition Audio controllers (`src/drivers/sound/`). `sound_init()` initializes both and prefers HDA; all operations dispatch to the active card. Playback is polled — neither driver uses interrupts.

## Common Model

- Stream configuration: sample rate, channels, bits.
- File playback by extension: `.wav` (PCM-only parser, RIFF chunk scan), `.mp3` (parser stub — not implemented), otherwise raw PCM.
- Buffer descriptor lists: 32 entries of 32 KB each in both drivers; a kernel sound buffer feeds them, and the poll function refills entries as the hardware consumes them.

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

`SYS_SOUND_WRITE` buffers feed the active card's play path; completion is observed by polling from the kernel side.
