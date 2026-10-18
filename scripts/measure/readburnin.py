#!/usr/bin/env python3
"""Read the corpus burn-in's frame number out of an image.

WHAT THIS IS FOR. The MP4 review's central accuracy question is "is the frame on
screen the frame that was asked for". Answering it needs the frame's identity to
be readable from the pixels, exactly, with no tolerance -- `delta 0` is a
contract in this project, not a tuned value, so the instrument that measures it
cannot be approximate.

WHY NOT OCR. A screenshot of a window carries scaling, a colour transform and
whatever the renderer's sampler did on the way. OCR on top of that is a second
source of error sitting in front of the measurement, and this project has
repeatedly paid for instruments that accuse a correct build. The strip is ten
boxes -- white = 1, black = 0, MSB left -- so reading it is a threshold on ten
sampled regions and nothing else.

HOW THE STRIP IS FOUND. Not at a constant offset: a player fits the picture to a
window, so the strip's position and size in a screenshot depend on the window.
The plate behind the bits is a known mid-grey (0x80) band running from the left
edge, so the reader locates the plate by its own colour and derives the ten cell
centres from the plate it measured. That is the same rule the overlay harnesses
in this repo already follow, and the reason is the same: a formula cannot notice
it has drifted.

The geometry constants below mirror make_mp4_corpus.ps1's Get-BurnIn. They are
expressed as FRACTIONS OF THE PLATE, so they survive any scaling between the
encoded frame and the screenshot.
"""

import argparse
import json
import sys

import numpy as np
from PIL import Image

# Mirrors Get-BurnIn, as fractions of the plate rather than pixels.
#   plateW = bitX0*2 + 10*bitW + 9*gap, with bitX0=60k, bitW=90k, gap=10k
#          = 120k + 900k + 90k = 1110k
#   plateH = bitH + 40k = 130k
# So a cell's width is 90/1110 of the plate, the first starts at 60/1110, and
# the pitch is 100/1110.
_PLATE_W_UNITS = 1110.0
_BIT_X0 = 60.0 / _PLATE_W_UNITS
_BIT_W = 90.0 / _PLATE_W_UNITS
_BIT_PITCH = 100.0 / _PLATE_W_UNITS
# Vertically the plate is 130k tall with the 90k cell starting 20k down.
_PLATE_H_UNITS = 130.0
_BIT_Y0 = 20.0 / _PLATE_H_UNITS
_BIT_H = 90.0 / _PLATE_H_UNITS

NBITS = 10


class BurnInError(RuntimeError):
    pass


def _find_plate(img):
    """Locate the grey plate's bounding box.

    Returns (x0, y0, x1, y1) inclusive-exclusive, or raises.

    THE ROW TEST ACCEPTS GREY *OR* WHITE, AND THAT IS NOT A LOOSENING -- it is
    the difference between an instrument that works and one that works for some
    frame numbers. The first version required a row to be >80% GREY across the
    left half of the picture. The white bit cells sit on those rows, so the grey
    fraction depends on HOW MANY BITS ARE SET: frame 13 put two cells in the
    sampled window and passed at 81%, frame 42 put three in and failed at 72%.
    On failure the run collapsed to the plate's bit-free top sliver, ten grey
    cells were sampled from above the bits, and the reader returned a confident
    `frame 0` -- for a subset of frames with no visible pattern, which is exactly
    what a landing bug looks like.

    A LONGEST-RUN TEST WAS TRIED AS A REPLACEMENT AND WAS WORSE. It removes the
    bit-pattern dependency in principle, and it failed 7 of the same 96 frames
    because the longest neutral run in a row is not always the plate -- content
    can beat it. Recorded so it is not re-attempted as an obvious improvement.

    The plate's ASPECT RATIO is asserted against the design's 130/1110, so a
    mislocation raises instead of returning a number. That guard has earned
    itself twice: on the bit-free sliver above, and on Trace's transport strip
    occluding the plate's lower half.
    """
    a = np.asarray(img.convert("RGB")).astype(np.int16)
    h, w, _ = a.shape

    spread = a.max(axis=2) - a.min(axis=2)
    level = a.mean(axis=2)
    # Grey: neutral and near 0x80. Tolerances survive a YUV round trip and
    # chroma subsampling while excluding the white cells and the black text box.
    grey = (spread <= 24) & (np.abs(level - 128) <= 26)
    # White: neutral and bright -- the bit cells.
    white = (spread <= 30) & (level > 200)
    plate_like = grey | white

    # Rows that are plate across the left half. The plate spans 1110/1920 of the
    # encoded width, so the left half is always inside it whatever the scaling.
    left = plate_like[:, : max(1, w // 2)]
    rows = np.flatnonzero(left.mean(axis=1) > 0.90)
    if rows.size == 0:
        raise BurnInError("no plate row found (is this a corpus clip?)")

    splits = np.flatnonzero(np.diff(rows) > 1)
    runs = np.split(rows, splits + 1)
    run = max(runs, key=len)
    y0, y1 = int(run[0]), int(run[-1]) + 1
    if (y1 - y0) < 6:
        raise BurnInError(f"plate too thin ({y1 - y0}px) -- picture too small to read")

    band = plate_like[y0:y1]
    cols = np.flatnonzero(band.mean(axis=0) > 0.70)
    if cols.size == 0:
        raise BurnInError("plate rows found but no plate columns")
    x0 = int(cols[0])
    x1 = x0
    for c in cols:
        if c - x1 > 2:
            break
        x1 = int(c)
    x1 += 1
    if (x1 - x0) < 40:
        raise BurnInError(f"plate too narrow ({x1 - x0}px) -- picture too small to read")

    aspect = (y1 - y0) / float(x1 - x0)
    expected = _PLATE_H_UNITS / _PLATE_W_UNITS
    if not (expected * 0.6 <= aspect <= expected * 1.6):
        raise BurnInError(
            f"plate aspect {aspect:.3f} is not the design's {expected:.3f} "
            f"(box {x0},{y0}-{x1},{y1}) -- plate mislocated, refusing to guess"
        )
    return x0, y0, x1, y1


def read_frame_number(path, debug=False):
    """Return (frame_number, info_dict). Raises BurnInError when unreadable.

    ROTATED PICTURES ARE DELIBERATELY NOT HANDLED, AND TRYING WAS WORSE THAN
    REFUSING. A version that attempted the capture at 0/90/180/270 looks like the
    obvious answer for a clip carrying a display matrix. It is not: the bit strip
    is left-to-right MSB-first, so a 180-degree rotation reverses the bit ORDER
    and the strip still reads as a valid plate. Frame 479 (0111011111) came back
    as **1006** (1111101110) -- a confident wrong number, which is strictly worse
    than an error, and the one outcome this whole instrument exists to avoid.

    Disambiguating would need an orientation marker baked into the plate, i.e. a
    corpus regeneration. Rotation is measured by eye from the human-readable
    `F <n>` burn-in instead, and the review records why that is sufficient:
    rotation is applied in the RENDERER, downstream of the frame, and
    `rotationDegrees` is read by neither `frameFromPts` nor `seekToFrame` -- so it
    cannot affect which frame an index resolves to.
    """
    img = Image.open(path)
    turn = 0
    x0, y0, x1, y1 = _find_plate(img)
    pw = x1 - x0
    ph = y1 - y0

    a = np.asarray(img.convert("RGB")).astype(np.float32)

    bits = []
    levels = []
    for i in range(NBITS):
        cx0 = x0 + _BIT_X0 * pw + i * _BIT_PITCH * pw
        cx1 = cx0 + _BIT_W * pw
        cy0 = y0 + _BIT_Y0 * ph
        cy1 = cy0 + _BIT_H * ph
        # Sample the middle 50% of each cell, so an edge landing a pixel off --
        # from scaling, or from the renderer's own filtering -- cannot reach the
        # sample.
        mx0 = int(round(cx0 + 0.25 * (cx1 - cx0)))
        mx1 = int(round(cx0 + 0.75 * (cx1 - cx0)))
        my0 = int(round(cy0 + 0.25 * (cy1 - cy0)))
        my1 = int(round(cy0 + 0.75 * (cy1 - cy0)))
        if mx1 <= mx0 or my1 <= my0:
            raise BurnInError("bit cell collapsed -- picture too small to read")
        cell = a[my0:my1, mx0:mx1]
        lvl = float(cell.mean())
        levels.append(lvl)
        bits.append(lvl)

    # Classify against the midpoint between the two populations rather than a
    # constant. A white cell is ~0xff and a grey one ~0x80 at source, but a
    # screenshot may carry a colour transform; what cannot change is that the
    # two populations stay far apart. Refuse to guess when they do not.
    lo, hi = min(levels), max(levels)
    if hi - lo < 40:
        # Every cell the same: either all bits set or none. Frame 0 is the only
        # legitimate all-zero, and 1023 the only all-one -- but a clip is under
        # 1024 frames so all-one cannot occur. So all-same means all-zero.
        if lo < 170:
            value = 0
            thresh = None
        else:
            raise BurnInError(
                f"bit cells indistinguishable and bright (lo={lo:.1f} hi={hi:.1f})"
            )
    else:
        thresh = (lo + hi) / 2.0
        value = 0
        for i, lvl in enumerate(levels):
            if lvl > thresh:
                value |= 1 << (NBITS - 1 - i)

    info = {
        "orientation": turn,
        "plate": [x0, y0, x1, y1],
        "levels": [round(v, 1) for v in levels],
        "threshold": None if thresh is None else round(thresh, 1),
    }
    if debug:
        info["bits"] = "".join(
            "1" if (thresh is not None and l > thresh) else "0" for l in levels
        )
    return value, info


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("images", nargs="+")
    ap.add_argument("--expect", type=int, default=None,
                    help="assert the frame number equals this; exit 1 if not")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--debug", action="store_true")
    args = ap.parse_args()

    rc = 0
    out = []
    for p in args.images:
        try:
            n, info = read_frame_number(p, debug=args.debug)
            rec = {"image": p, "frame": n, "ok": True,
                   "orientation": info.get("orientation", 0)}
            if args.debug:
                rec.update(info)
            if args.expect is not None:
                rec["expect"] = args.expect
                rec["match"] = (n == args.expect)
                if n != args.expect:
                    rc = 1
            out.append(rec)
            if not args.json:
                extra = ""
                if info.get("orientation"):
                    extra += f"  [read at {info['orientation']} deg]"
                if args.debug:
                    extra += f"  bits={info.get('bits')} levels={info['levels']}"
                verdict = ""
                if args.expect is not None:
                    verdict = "  MATCH" if n == args.expect else f"  MISMATCH (want {args.expect})"
                print(f"{p}: frame {n}{verdict}{extra}")
        except BurnInError as e:
            out.append({"image": p, "ok": False, "error": str(e)})
            rc = 2
            if not args.json:
                print(f"{p}: UNREADABLE -- {e}", file=sys.stderr)
    if args.json:
        print(json.dumps(out, indent=2))
    return rc


if __name__ == "__main__":
    sys.exit(main())
