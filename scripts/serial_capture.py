"""Serial capture with per-line timestamps for 一丢口语 stutter diagnosis.

Each line is written as:
    HH:MM:SS.mmm [+seconds.since_start] [gap=..ms] <raw line>

`gap` is the idle gap before this line arrived; it makes playback/network
stalls visible when reading the log afterwards.

Usage:
    python serial_capture.py --port COM12 --baud 115200 --out oral_cap.log
Stop with Ctrl-C or by killing the process.
"""

import argparse
import datetime
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="COM12")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--out", required=True)
    ap.add_argument("--max-seconds", type=float, default=3600.0)
    args = ap.parse_args()

    start = time.monotonic()
    last_line_at = None
    buf = b""
    total = 0
    reopened = 0

    out = open(args.out, "w", encoding="utf-8", buffering=1)
    out.write("# capture port=%s baud=%d start=%s\n"
              % (args.port, args.baud,
                 datetime.datetime.now().isoformat(timespec="milliseconds")))

    port = None
    while time.monotonic() - start < args.max_seconds:
        if port is None:
            try:
                port = serial.Serial(args.port, args.baud, timeout=0.5)
            except Exception as exc:  # device may be mid re-enumeration
                out.write("# open failed: %s\n" % exc)
                time.sleep(1.0)
                continue

        try:
            chunk = port.read(4096)
        except Exception as exc:
            out.write("# read failed: %s\n" % exc)
            try:
                port.close()
            except Exception:
                pass
            port = None
            continue

        if not chunk:
            continue
        buf += chunk
        while b"\n" in buf:
            raw, buf = buf.split(b"\n", 1)
            now = time.monotonic()
            text = raw.decode("utf-8", "replace").rstrip("\r")
            gap = 0.0 if last_line_at is None else (now - last_line_at) * 1000.0
            last_line_at = now
            stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            out.write("%s [+%8.3f] [gap=%6.0f] %s\n"
                      % (stamp, now - start, gap, text))
            total += 1
            if total % 200 == 0:
                out.flush()
        out.flush()

    if port is not None:
        port.close()
    out.write("# capture end lines=%d reopened=%d\n" % (total, reopened))
    out.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
