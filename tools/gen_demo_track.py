#!/usr/bin/env python3
"""Deterministic stdlib-only synthesizer for the demo track (rootfs/Music/demo.wav).

Fixed pentatonic arpeggio, each note a decaying sine; no RNG anywhere, so the
output is byte-identical across runs and machines.
"""
from __future__ import annotations

import argparse
import math
import struct

ARPEGGIO = (440.000, 554.365, 659.255, 739.989, 880.000)  # A4 C#5 E5 F#5 A5
NOTE_INTERVAL = 0.5  # seconds between note onsets
NOTE_DECAY = 1.2  # seconds until the envelope has fallen to ~1%
NOTE_ATTACK = 0.004  # per-note click guard, seconds
NOTE_AMP = 0.45  # worst-case overlap keeps the track peak under 0.6
FADE_IN = 0.03  # whole-track linear fade-in, seconds
FADE_OUT = 0.4  # whole-track linear fade-out, seconds
CHANNEL_DELAY = 6  # right-channel delay in samples, for stereo width


def note_schedule(seconds: float) -> list[tuple[float, float]]:
    """(onset, frequency) pairs: the arpeggio repeated, every other cycle an octave down."""
    notes = []
    i = 0
    while i * NOTE_INTERVAL < seconds:
        cycle, step = divmod(i, len(ARPEGGIO))
        octave = 0.5 if cycle % 2 == 1 else 1.0
        notes.append((i * NOTE_INTERVAL, ARPEGGIO[step] * octave))
        i += 1
    return notes


def synthesize(seconds: float, rate: int) -> tuple[list[float], list[float]]:
    total = int(round(seconds * rate))
    mono = [0.0] * total
    for onset, freq in note_schedule(seconds):
        first = int(round(onset * rate))
        length = int(round(NOTE_DECAY * rate))
        two_pi_freq = 2.0 * math.pi * freq
        for j in range(length):
            n = first + j
            if n >= total:
                break
            t = j / rate
            envelope = min(t / NOTE_ATTACK, 1.0) * math.exp(-5.0 * t / NOTE_DECAY)
            mono[n] += NOTE_AMP * envelope * math.sin(two_pi_freq * t)

    fade_in = FADE_IN * rate
    fade_out = FADE_OUT * rate
    left = [0.0] * total
    right = [0.0] * total
    for n in range(total):
        gain = 1.0
        if n < fade_in:
            gain *= n / fade_in
        if n >= total - fade_out:
            gain *= (total - 1 - n) / fade_out
        left[n] = mono[n] * gain
        right[n] = mono[n - CHANNEL_DELAY] * gain if n >= CHANNEL_DELAY else 0.0
    return left, right


def write_wav(path: str, seconds: float, rate: int) -> None:
    left, right = synthesize(seconds, rate)
    total = len(left)
    frames = []
    for n in range(total):
        for x in (left[n], right[n]):
            value = max(-32768, min(32767, int(round(x * 32767.0))))
            frames.append(value)
    data = struct.pack(f'<{len(frames)}h', *frames)

    channels = 2
    bits = 16
    block_align = channels * bits // 8
    data_size = len(data)
    header = b'RIFF' + struct.pack('<I', 36 + data_size) + b'WAVE'
    header += b'fmt ' + struct.pack('<IHHIIHH', 16, 1, channels, rate, rate * block_align, block_align, bits)
    header += b'data' + struct.pack('<I', data_size)

    with open(path, 'wb') as out:
        out.write(header)
        out.write(data)


def main() -> int:
    parser = argparse.ArgumentParser(description="Synthesize the deterministic demo WAV track.")
    parser.add_argument('--output', required=True)
    parser.add_argument('--seconds', type=float, default=10.0)
    parser.add_argument('--rate', type=int, default=44100)
    args = parser.parse_args()
    write_wav(args.output, args.seconds, args.rate)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
