"""Estimate median F0 of speech in a WAV via autocorrelation.

Female speech F0 is typically 180-260 Hz; male 90-150 Hz.
If a 16 kHz stream is played back as 24 kHz, every F0 is multiplied by 1.5
(e.g. 200 Hz -> 300 Hz), which is the signature of a sample-rate mix-up.
"""

import sys
import wave

import numpy as np


def f0_track(x, rate, fmin=70, fmax=450, hop=0.02, win=0.04):
    n_hop = int(rate * hop)
    n_win = int(rate * win)
    out = []
    for start in range(0, len(x) - n_win, n_hop):
        seg = x[start:start + n_win]
        seg = seg - seg.mean()
        if np.sqrt((seg ** 2).mean()) < 200:
            continue
        corr = np.correlate(seg, seg, mode="full")[n_win - 1:]
        lo = int(rate / fmax)
        hi = int(rate / fmin)
        if hi >= len(corr):
            continue
        peak = np.argmax(corr[lo:hi]) + lo
        if peak <= 0:
            continue
        strength = corr[peak] / (corr[0] + 1e-9)
        if strength < 0.25:
            continue
        out.append(rate / peak)
    return np.array(out)


def main(path):
    with wave.open(path, "rb") as w:
        rate = w.getframerate()
        ch = w.getnchannels()
        data = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
    if ch == 2:
        data = data[::2]
    f0 = f0_track(data, rate)
    if f0.size < 5:
        print(f"{path}: not enough voiced frames")
        return
    med = float(np.median(f0))
    print(f"{path}: frames={f0.size}  median F0={med:.1f} Hz  "
          f"p10={np.percentile(f0,10):.0f} p90={np.percentile(f0,90):.0f}")
    if med > 300:
        print("  => F0 unusually high: consistent with 1.5x speed-up "
              "(16 kHz stream treated as 24 kHz)")
    elif med < 260:
        print("  => F0 in normal range: playback rate likely correct")
    else:
        print("  => borderline")


if __name__ == "__main__":
    for p in sys.argv[1:]:
        main(p)
