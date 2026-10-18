#!/usr/bin/env python3
"""Does Trace's frame-index model lose frames on this file?

Trace maps between a frame index and a timestamp with ONE assumption, in both
directions (VideoDecoderFFmpeg.cpp:1672 and :2037):

    index = round((pts - stream_start) * fps)
    pts   = stream_start + index / fps

On constant-rate material that is the same thing as "the Nth frame" and
everything agrees. This script applies exactly that arithmetic to a file's real
timestamps and reports what it costs:

    COLLISION   two or more source frames round to the same index. All but one
                of them is then UNREACHABLE -- no index the user can ask for
                will ever display it.
    GAP         an index inside the file's range that no source frame rounds to.
                Asking for it displays a neighbour.

It is deliberately PURE ARITHMETIC ON THE CONTAINER and does not involve Trace
at all, so it is an independent prediction rather than a measurement of the
thing being tested. A file this reports as clean cannot demonstrate the fault,
and saying so is the point: a fixture that cannot fail proves nothing when it
passes.

WHY THE OFFSET BUDGET IS HALF A FRAME. round() moves to a different integer only
once a timestamp is more than half a frame period from its nominal slot. So a
file whose timing jitter is +-0.35 of a frame is genuinely variable-rate and
still maps perfectly -- which is a real and useful result, not a weak fixture,
as long as nobody reads its pass as covering the other case.
"""

import argparse
import csv
import os
import subprocess
import sys
from fractions import Fraction

FFPROBE = r"C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffprobe.exe"


def stream_info(path):
    out = subprocess.run(
        [FFPROBE, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "stream=r_frame_rate,avg_frame_rate,start_time,nb_frames",
         "-of", "csv=p=0", path],
        capture_output=True, text=True).stdout.strip().split(",")
    r_rate, avg_rate, start, nb = (out + ["", "", "", ""])[:4]
    return r_rate, avg_rate, start, nb


def frame_times(path):
    out = subprocess.run(
        [FFPROBE, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "frame=pts_time", "-of", "csv=p=0", path],
        capture_output=True, text=True).stdout
    ts = []
    for row in csv.reader(out.splitlines()):
        if not row:
            continue
        try:
            ts.append(float(row[0]))
        except Exception:
            pass
    return sorted(ts)


def analyse(path, rate_override=None):
    r_rate, avg_rate, start, nb = stream_info(path)
    # av_guess_frame_rate prefers r_frame_rate for most containers; that is the
    # value Trace ends up with, so it is the one modelled here. The override
    # exists to show what the other choice would cost.
    rate_str = rate_override or r_rate
    try:
        fps = float(Fraction(rate_str))
    except Exception:
        return None
    try:
        start_s = float(start)
    except Exception:
        start_s = 0.0

    ts = frame_times(path)
    if not ts or fps <= 0:
        return None

    idx = [round((t - start_s) * fps) for t in ts]
    offsets = [abs(((t - start_s) * fps) - round((t - start_s) * fps)) for t in ts]

    seen = {}
    collisions = 0
    for i in idx:
        seen[i] = seen.get(i, 0) + 1
    for k, v in seen.items():
        if v > 1:
            collisions += v - 1

    lo, hi = min(idx), max(idx)
    gaps = sum(1 for i in range(lo, hi + 1) if i not in seen)

    return {
        "file": os.path.basename(path),
        "frames": len(ts),
        "nb_frames": nb,
        "rate": rate_str,
        "avg_rate": avg_rate,
        "start": start_s,
        "index_lo": lo,
        "index_hi": hi,
        "distinct": len(seen),
        "collisions": collisions,
        "gaps": gaps,
        "max_offset": max(offsets),
        "unreachable": collisions,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--rate", default=None,
                    help="model a different nominal rate (e.g. the avg instead of r)")
    args = ap.parse_args()

    files = []
    for p in args.paths:
        if os.path.isdir(p):
            for e in sorted(os.listdir(p)):
                if e.lower().endswith((".mp4", ".mov", ".mkv")):
                    files.append(os.path.join(p, e))
        else:
            files.append(p)

    hdr = (f"{'FILE':<34}{'FRAMES':>7}{'DISTINCT':>9}{'COLLIDE':>8}"
           f"{'GAPS':>6}{'MAXOFF':>8}  VERDICT")
    print(hdr)
    print("-" * (len(hdr) + 24))
    rc = 0
    for f in files:
        d = analyse(f, args.rate)
        if not d:
            print(f"{os.path.basename(f):<34}  (no video / unreadable)")
            continue
        name = d["file"]
        if len(name) > 33:
            name = name[:30] + "..."
        if d["collisions"] or d["gaps"]:
            verdict = f"LOSES {d['unreachable']} frame(s), {d['gaps']} empty index/es"
            rc = 1
        elif d["max_offset"] > 0.25:
            verdict = "clean, but within 2x of the half-frame limit"
        else:
            verdict = "clean"
        print(f"{name:<34}{d['frames']:>7}{d['distinct']:>9}{d['collisions']:>8}"
              f"{d['gaps']:>6}{d['max_offset']:>8.3f}  {verdict}")
    return rc


if __name__ == "__main__":
    sys.exit(main())
