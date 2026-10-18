#!/usr/bin/env python3
"""GOP structure of a set of MP4s, from the container's own packet flags.

Exists because the MP4 review's central question -- which file properties
predict bad behaviour -- needs the properties measured rather than taken from
the encoder command line. An encoder is asked for `-g 48`; what lands in the
container is what a scene-cut decision, an open-GOP setting and the muxer
between them produced.

KEYFRAMES ARE COUNTED ON THE EXACT flags FIELD, NOT BY SUBSTRING. ffprobe's
packet flags read like "K__"; a loose case-insensitive search for "K" also
matches the k in "packet", which is a trap this repo has already paid for -- a
GOP probe that called every file all-keyframe.
"""

import argparse
import csv
import glob
import os
import subprocess
import sys

FFPROBE = r"C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffprobe.exe"


def probe(path):
    out = subprocess.run(
        [FFPROBE, "-v", "error", "-select_streams", "v:0",
         "-show_entries", "packet=pts_time,dts_time,flags",
         "-of", "csv=p=0", path],
        capture_output=True, text=True)
    rows = [r for r in csv.reader(out.stdout.splitlines()) if r]
    pts, dts, keys = [], [], []
    for i, r in enumerate(rows):
        # csv order follows -show_entries: pts_time, dts_time, flags
        def f(v):
            try:
                return float(v)
            except Exception:
                return None
        if len(r) >= 3:
            pts.append(f(r[0]))
            dts.append(f(r[1]))
            flags = r[2]
        elif len(r) == 2:
            pts.append(f(r[0]))
            dts.append(None)
            flags = r[1]
        else:
            continue
        # Exact field, first character.
        if flags.startswith("K"):
            keys.append(i)

    gaps = [keys[i + 1] - keys[i] for i in range(len(keys) - 1)]
    clean_pts = [p for p in pts if p is not None]
    clean_dts = [d for d in dts if d is not None]

    # PTS out of order in STORAGE order is the signature of B-frame reordering:
    # the container stores frames in decode order, and a reordered stream has a
    # frame whose presentation time precedes its predecessor's.
    reordered = any(clean_pts[i] > clean_pts[i + 1] for i in range(len(clean_pts) - 1))
    # DTS must always be monotonic; if it is not, the file is malformed rather
    # than merely reordered, and that is worth saying separately.
    dts_ok = all(clean_dts[i] <= clean_dts[i + 1] for i in range(len(clean_dts) - 1)) if clean_dts else True

    # Is the spacing between presentation times uniform? This is the VFR test,
    # done on timestamps rather than on the container's avg/r_frame_rate pair.
    deltas = []
    sp = sorted(clean_pts)
    for i in range(len(sp) - 1):
        deltas.append(sp[i + 1] - sp[i])
    jitter = 0.0
    if deltas:
        mean = sum(deltas) / len(deltas)
        if mean > 0:
            jitter = max(abs(d - mean) for d in deltas) / mean

    return {
        "frames": len(pts),
        "keyframes": len(keys),
        "max_gop": max(gaps) if gaps else len(pts),
        "mean_gop": round(sum(gaps) / len(gaps), 1) if gaps else len(pts),
        "first_key": keys[0] if keys else -1,
        "reordered": reordered,
        "dts_monotonic": dts_ok,
        "pts_jitter": round(jitter, 4),
        "first_pts": round(clean_pts[0], 4) if clean_pts else None,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", help="files or globs")
    args = ap.parse_args()

    files = []
    for p in args.paths:
        if os.path.isdir(p):
            files += sorted(glob.glob(os.path.join(p, "*.mp4")))
            files += sorted(glob.glob(os.path.join(p, "*.mov")))
            files += sorted(glob.glob(os.path.join(p, "*.MOV")))
        else:
            g = sorted(glob.glob(p))
            files += g if g else [p]

    hdr = (f"{'FILE':<34}{'FRAMES':>7}{'KEYS':>6}{'MAXGOP':>7}"
           f"{'MEANGOP':>8}{'1STKEY':>7}{'REORD':>7}{'JITTER':>8}{'PTS0':>8}")
    print(hdr)
    print("-" * len(hdr))
    for f in files:
        if not os.path.exists(f):
            print(f"{os.path.basename(f):<34}  MISSING")
            continue
        try:
            d = probe(f)
        except Exception as e:
            print(f"{os.path.basename(f):<34}  ERROR {e}")
            continue
        name = os.path.basename(f)
        if len(name) > 33:
            name = name[:30] + "..."
        flag = "yes" if d["reordered"] else "no"
        if not d["dts_monotonic"]:
            flag += "!dts"
        print(f"{name:<34}{d['frames']:>7}{d['keyframes']:>6}{d['max_gop']:>7}"
              f"{d['mean_gop']:>8}{d['first_key']:>7}{flag:>7}"
              f"{d['pts_jitter']:>8}{str(d['first_pts']):>8}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
