#!/usr/bin/env python3
"""Validate readburnin.py against a whole clip, exhaustively, with Trace absent.

WHY THIS EXISTS AS ITS OWN TOOL. readburnin.py is the instrument the entire MP4
accuracy matrix rests on: every "the frame on screen is not the frame asked for"
verdict in this project is that reader's opinion of a screenshot. An instrument
like that has to be checked against ground truth separately from the thing it
measures, or a reader fault and a player fault are indistinguishable -- and this
project has now had both, in that order, on the same afternoon.

It decodes a clip frame by frame with ffmpeg, so the index of each frame is
known WITHOUT asking Trace anything, and reads the burn-in out of it. The burned
number was drawn by the generator from the same counter, so for every frame the
answer is known in advance and a disagreement is the reader's.

THE DOWNSCALE IS PART OF THE TEST, not a convenience. A window capture is never
the encoded frame: a 4K clip fitted into a 1280-wide viewer reaches the reader
at a third of its size, through the renderer's own sampler. Reading the full-size
frame would validate the reader on material it never sees.
"""

import argparse
import os
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import readburnin  # noqa: E402

FFMPEG = r"C:\Users\andre\AppData\Local\Microsoft\WinGet\Packages\Gyan.FFmpeg_Microsoft.Winget.Source_8wekyb3d8bbwe\ffmpeg-9.0-full_build\bin\ffmpeg.exe"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("clip")
    ap.add_argument("--width", type=int, default=1280,
                    help="decode width; models the viewer the capture comes from")
    ap.add_argument("--start", type=int, default=0)
    ap.add_argument("--count", type=int, default=0, help="0 = to the end")
    ap.add_argument("--keep", default="", help="directory to keep the PNGs in")
    args = ap.parse_args()

    out = args.keep or tempfile.mkdtemp(prefix="checkburnin_")
    os.makedirs(out, exist_ok=True)

    # One decode pass, every frame, named by its ORDINAL POSITION in the output
    # -- which is what the generator's %{frame_num} counted. The numbering comes
    # from image2's own %d counter rather than from -frame_pts: pts-derived
    # filenames are a DIFFERENT quantity from the ordinal, and taking one for
    # the other is how this check was first read backwards.
    vf = f"scale={args.width}:-2"
    if args.count:
        vf = f"select='between(n\\,{args.start}\\,{args.start + args.count - 1})'," + vf
    elif args.start:
        vf = f"select='gte(n\\,{args.start})'," + vf
    cmd = [FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-i", args.clip,
           "-vf", vf, "-fps_mode", "passthrough",
           "-start_number", str(args.start), os.path.join(out, "f_%05d.png")]
    subprocess.run(cmd, check=True)

    files = sorted(f for f in os.listdir(out) if f.startswith("f_") and f.endswith(".png"))
    if not files:
        print("FAIL: ffmpeg produced no frames", file=sys.stderr)
        return 2

    bad = []
    unreadable = []
    for name in files:
        idx = int(name[2:-4])
        try:
            value, _ = readburnin.read_frame_number(os.path.join(out, name))
        except readburnin.BurnInError as exc:
            unreadable.append((idx, str(exc)))
            continue
        if value != idx:
            bad.append((idx, value))

    n = len(files)
    print(f"clip      : {args.clip}")
    print(f"decoded   : {n} frames at width {args.width}")
    print(f"mismatch  : {len(bad)}")
    print(f"unreadable: {len(unreadable)}")
    for idx, value in bad[:20]:
        print(f"   frame {idx} read as {value}")
    for idx, msg in unreadable[:20]:
        print(f"   frame {idx} UNREADABLE: {msg}")

    # A run that decoded nothing must not pass. The zero-comparison failure is
    # this repo's recurring one and it has already reached this measurement
    # once via a frame-count mismatch that made a loop iterate zero times.
    if n == 0:
        print("RESULT: FAIL (no frames compared)")
        return 2
    ok = not bad and not unreadable
    print(f"RESULT: {'PASS' if ok else 'FAIL'} -- {n} frames compared")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
