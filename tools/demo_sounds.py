#!/usr/bin/env python3
"""Example loops for the README screenshots: four 2-bar loops at 120 bpm
(4.0 s, 16-bit 44.1 kHz stereo), synthesized here so nothing is borrowed.

    tools/demo_sounds.py OUTDIR    writes drums.wav, bass.wav, chords.wav, pad.wav
"""
import os
import sys
import wave

import numpy as np

SR = 44100
BEAT = 0.5                    # 120 bpm
N = int(SR * BEAT * 8)        # two bars of 4/4
rng = np.random.default_rng(7)


def env(n, decay):
    return np.exp(-np.arange(n) / (SR * decay))


def place(buf, sig, t):
    i = int(t * SR)
    n = min(len(sig), len(buf) - i)
    buf[i:i + n] += sig[:n]


def onepole(x, a):            # a one-pole low-pass, a = 0..1 (higher: brighter)
    y = np.empty_like(x)
    z = 0.0
    for i, v in enumerate(x):
        z += a * (v - z)
        y[i] = z
    return y


def saw(f, n, detune=0.0):
    t = np.arange(n) / SR
    return 2.0 * ((t * f * (1.0 + detune)) % 1.0) - 1.0


def note(m):
    return 440.0 * 2.0 ** ((m - 69) / 12.0)


def drums():
    L = np.zeros(N)
    R = np.zeros(N)
    kn = int(0.35 * SR)
    t = np.arange(kn) / SR
    f = 45.0 + 80.0 * np.exp(-t / 0.04)
    kick = np.sin(2 * np.pi * np.cumsum(f) / SR) * env(kn, 0.12)
    sn = int(0.25 * SR)
    snare = (rng.standard_normal(sn) * 0.6 + np.sin(2 * np.pi * 185 * np.arange(sn) / SR)) * env(sn, 0.06)
    hn = int(0.06 * SR)
    hat = np.diff(rng.standard_normal(hn + 1)) * env(hn, 0.012) * 0.5
    ohat = np.diff(rng.standard_normal(int(0.3 * SR) + 1)) * env(int(0.3 * SR), 0.08) * 0.35
    for b in range(8):
        place(L, kick, b * BEAT)
        place(R, kick, b * BEAT)
        if b % 2 == 1:
            place(L, snare * 0.8, b * BEAT)
            place(R, snare * 0.8, b * BEAT)
    place(L, kick * 0.7, 6.75 * BEAT)
    place(R, kick * 0.7, 6.75 * BEAT)
    for e in range(16):
        h = ohat if e in (7, 15) else hat
        place(L, h * 0.6, e * BEAT / 2)
        place(R, h, e * BEAT / 2)
    return L, R


def bass():
    seq = [45, 0, 45, 57, 0, 45, 43, 0, 41, 0, 41, 53, 0, 41, 43, 45]   # A minor, eighths
    x = np.zeros(N)
    nn = int(BEAT / 2 * SR)
    gate = np.minimum(1.0, (nn - np.arange(nn)) / (0.01 * SR))   # a short release at the note's end
    for i, m in enumerate(seq):
        if m:
            s = saw(note(m), nn) * (0.55 + 0.45 * env(nn, 0.08)) * gate
            place(x, s, i * BEAT / 2)
    x = onepole(onepole(x, 0.08), 0.12)
    x /= np.max(np.abs(x))
    return x, x


def chords():
    prog = [[57, 60, 64, 67], [53, 57, 60, 64]]   # Am7, Fmaj7
    L = np.zeros(N)
    R = np.zeros(N)
    nn = int(0.45 * SR)
    for bar, ch in enumerate(prog):
        for off in (0.5, 1.5, 2.5, 3.25):   # off-beat stabs
            sl = sum(saw(note(m), nn, -0.003) for m in ch) * env(nn, 0.22)
            sr = sum(saw(note(m), nn, 0.003) for m in ch) * env(nn, 0.22)
            place(L, sl, (bar * 4 + off) * BEAT)
            place(R, sr, (bar * 4 + off) * BEAT)
    L = onepole(L, 0.2)
    R = onepole(R, 0.2)
    m = max(np.max(np.abs(L)), np.max(np.abs(R)))
    return L / m, R / m


def pad():
    t = np.arange(N) / SR
    L = np.zeros(N)
    R = np.zeros(N)
    for half, ch in enumerate([[45, 57, 64, 72], [41, 53, 60, 69]]):
        seg = slice(half * N // 2, (half + 1) * N // 2)
        for m in ch:
            L[seg] += np.sin(2 * np.pi * note(m) * 0.998 * t[seg]) + 0.3 * np.sin(4 * np.pi * note(m) * t[seg])
            R[seg] += np.sin(2 * np.pi * note(m) * 1.002 * t[seg]) + 0.3 * np.sin(4 * np.pi * note(m) * t[seg])
    ph = (t % (N / 2 / SR)) / (N / 2 / SR)        # each chord swells and fades
    swell = np.sin(np.pi * ph) ** 1.5
    pan = 0.5 + 0.35 * np.sin(2 * np.pi * t / (N / SR))
    L *= swell * (1.0 - pan) * 2
    R *= swell * pan * 2
    m = max(np.max(np.abs(L)), np.max(np.abs(R)))
    return L / m, R / m


def write(path, lr, level):
    L, R = lr
    pk = max(np.max(np.abs(L)), np.max(np.abs(R)))
    st = np.stack([L, R], axis=1) / pk * level
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes((st * 32767).astype("<i2").tobytes())


if __name__ == "__main__":
    out = sys.argv[1] if len(sys.argv) > 1 else "."
    os.makedirs(out, exist_ok=True)
    write(os.path.join(out, "drums.wav"), drums(), 0.8)
    write(os.path.join(out, "bass.wav"), bass(), 0.7)
    write(os.path.join(out, "chords.wav"), chords(), 0.55)
    write(os.path.join(out, "pad.wav"), pad(), 0.5)
