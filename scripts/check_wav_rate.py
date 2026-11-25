"""Estimate the true sample rate of a PCM WAV by looking at spectral content.

If the file claims 24 kHz but was actually produced at 16 kHz, there will be
almost no energy above ~8 kHz (and a hard cliff there). If it is genuinely
24 kHz speech, energy extends up to ~10-12 kHz.
"""

import sys
import wave

import numpy as np


def main(path):
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        frames = w.readframes(w.getnframes())
        channels = w.getnchannels()
    data = np.frombuffer(frames, dtype="<i2").astype(np.float64)
    if channels == 2:
        data = data[::2]
    if data.size < 2048:
        print("too short")
        return
    data -= data.mean()
    win = np.hanning(min(16384, data.size))
    seg = data[: win.size] * win
    spec = np.abs(np.fft.rfft(seg))
    freqs = np.fft.rfftfreq(seg.size, 1.0 / rate)
    total = spec.sum()
    bands = [(0, 1000), (1000, 2000), (2000, 4000), (4000, 6000),
             (6000, 7000), (7000, 8000), (8000, 10000), (10000, 12000)]
    print(f"{path}  rate={rate} Hz  dur={data.size / rate:.2f}s")
    for lo, hi in bands:
        m = (freqs >= lo) & (freqs < hi)
        pct = 100.0 * spec[m].sum() / total if total else 0
        bar = "#" * int(pct / 2)
        print(f"  {lo:5d}-{hi:5d} Hz : {pct:5.1f}%  {bar}")
    hi_mask = freqs >= 7500
    print(f"  energy above 7.5 kHz: {100.0 * spec[hi_mask].sum() / total:.2f}%")
    print("  => " + ("likely 16 kHz source labelled higher"
                     if 100.0 * spec[hi_mask].sum() / total < 1.0
                     else "consistent with 24 kHz source"))


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
