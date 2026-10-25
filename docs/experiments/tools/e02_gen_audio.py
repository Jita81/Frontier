#!/usr/bin/env python3
"""E2: generate replacement audio from scratch (no third-party material).

Writes to <outdir>:
  sfx_00.wav  - 22050 Hz 16-bit mono UI click replacement (rising chirp)
  01.wav      - ~10 s procedural synth piece, 22050 Hz 16-bit stereo
Convert 01.wav to the 01.ogg the game loads with:
  ffmpeg -y -i 01.wav -ar 22050 -ac 2 -c:a vorbis -strict -2 01.ogg
Everything here is synthesised from sine/square maths, so the output is
original work licensed with this repository.
"""
import math, struct, sys, wave

RATE = 22050

def write_wav(path, frames, channels):
    with wave.open(path, "wb") as w:
        w.setnchannels(channels); w.setsampwidth(2); w.setframerate(RATE)
        w.writeframes(b"".join(struct.pack("<h", max(-32767, min(32767, int(s)))) for s in frames))

def chirp():
    n = int(RATE * 0.09); out = []; ph = 0.0
    for i in range(n):
        t = i / n
        f = 900 + 1700 * t                      # 900 -> 2600 Hz sweep
        ph += 2 * math.pi * f / RATE
        env = min(1.0, i / 40) * (1 - t) ** 2   # click attack, fast decay
        out.append(14000 * env * (math.sin(ph) + 0.3 * math.sin(3 * ph)))
    return out

def piece():
    # A minor arpeggio over a slow bass, 96 bpm, 32 eighth notes (= 10 s).
    notes = [57, 60, 64, 69, 64, 60, 57, 60,  53, 57, 60, 65, 60, 57, 53, 57,
             55, 59, 62, 67, 62, 59, 55, 59,  52, 56, 59, 64, 59, 56, 52, 56]
    bass = [45, 41, 43, 40]
    step = 60 / 96 / 2                           # eighth notes
    n = int(RATE * step * len(notes)); L = []; R = []
    hz = lambda m: 440 * 2 ** ((m - 69) / 12)
    for i in range(n):
        t = i / RATE
        k = int(t / step) % len(notes); tk = t - int(t / step) * step
        lead = math.sin(2 * math.pi * hz(notes[k]) * t) * math.exp(-tk * 6)
        b = bass[int(t / (step * 8)) % 4]
        low = math.sin(2 * math.pi * hz(b) * t) * 0.6
        fade = min(1.0, t / 0.5, (n / RATE - t) / 1.5)
        L.append(9000 * fade * (lead * 0.8 + low))
        R.append(9000 * fade * (lead * 0.5 + low))
    return [s for pair in zip(L, R) for s in pair]

if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    write_wav(f"{out}/sfx_00.wav", chirp(), 1)
    write_wav(f"{out}/01.wav", piece(), 2)
