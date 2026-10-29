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

WITH ONE EXCEPTION, ADDED 2026-10-29, AND IT IS THE OPPOSITE LESSON. The plate's
RIGHT EDGE is no longer measured -- it is derived from the measured height and
the design's own 1110:130 ratio. Measuring it meant scanning rightward until the
grey ran out, which silently assumes the picture beside the plate is not itself
neutral grey. On synthetic testsrc2 footage that holds. On real video it does
not: a street scene panning onto ASPHALT continued the run past the plate, the
box came out too wide, the ten cell centres were spread over it, and the reader
returned confident wrong numbers for a content-dependent subset of frames --
which is indistinguishable from a landing bug, and was read as one until the
frames were pulled out of the file and looked at. See _find_plate.

The general rule survives and is sharpened: measure what the picture can tell
you reliably, derive what it cannot, and make the thing you no longer measure
into a GUARD rather than discarding it.

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
    scanned_x1 = x0
    for c in cols:
        if c - scanned_x1 > 2:
            break
        scanned_x1 = int(c)
    scanned_x1 += 1

    # THE RIGHT EDGE IS DERIVED FROM THE HEIGHT, NOT SCANNED FOR, AND THAT IS
    # THE FIX FOR REAL FOOTAGE.
    #
    # Scanning right until the plate-like run breaks works perfectly on the
    # synthetic corpus and FAILS SILENTLY on real video, because it assumes the
    # picture beside the plate is not itself neutral grey. Asphalt is. Measured
    # on IMG_0516 (a street scene that pans onto a road): the run continued past
    # the plate's true right edge into the road surface, the box came out too
    # wide, the ten cell centres were spread across it, and the reader returned
    # 504 for a frame whose plate -- confirmed by eye from the same capture's
    # human-readable `F 510` and by counting its eight lit cells -- says 510.
    # Frames 0..257 read correctly because the camera was pointed elsewhere, so
    # the failures were a content-dependent subset with no visible pattern:
    # EXACTLY what a landing bug looks like, and it was read as one until the
    # frames were pulled out of the file with ffmpeg and looked at.
    #
    # The old aspect guard could not catch it. Widening 2220 -> ~2900 takes the
    # aspect from 0.117 to ~0.090, which is inside the 0.6-1.6x tolerance that
    # same guard needs in order to survive scaling.
    #
    # The height is reliable where the width is not: the row test demands >90%
    # plate-like across the LEFT HALF OF THE PICTURE, which content has to work
    # much harder to satisfy than a single column does. So the width comes from
    # the height and the design's own ratio, and nothing is scanned into.
    expected_ratio = _PLATE_W_UNITS / _PLATE_H_UNITS
    x1 = x0 + int(round((y1 - y0) * expected_ratio))
    if (x1 - x0) < 40:
        raise BurnInError(f"plate too narrow ({x1 - x0}px) -- picture too small to read")
    if x1 > w:
        raise BurnInError(
            f"plate derived as {x0}..{x1} but the image is only {w}px wide "
            f"-- plate mislocated, refusing to guess"
        )
    # The scan is kept as the OCCLUSION guard, which is the one thing it is good
    # at: something covering the plate's right-hand end makes the run stop
    # EARLY, and no amount of grey content can fake that. Trace's own
    # edge-to-edge transport strip is the recorded case.
    if scanned_x1 < x0 + 0.9 * (x1 - x0):
        raise BurnInError(
            f"plate runs out at {scanned_x1} but the design needs {x1} "
            f"(box {x0},{y0}-{x1},{y1}) -- plate occluded, refusing to guess"
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
        # EVERY CELL MUST BE OF ONE COLOUR, and this is the guard that replaces
        # what the aspect test used to be doing.
        #
        # A correctly located cell is a solid fill -- the generator draws it with
        # drawbox t=fill -- so it is either all grey plate or all white bit. A
        # MISLOCATED one straddles a boundary and is a mixture, which is the
        # visible signature of the plate having been found in the wrong place.
        # Checked per cell rather than on the box as a whole because a box can
        # be the right size and the wrong shape, and because this is the only
        # test that sees where the samples ACTUALLY landed.
        #
        # It catches the vertical case specifically, which the derived width
        # cannot: a plate whose measured height is short (occluded, or a
        # contaminated row run) yields a proportionally narrow box that still
        # sits entirely inside the real plate and passes every box-level test,
        # while the cells are sampled from rows above or below the bits.
        cspread = cell.max(axis=2) - cell.min(axis=2)
        clevel = cell.mean(axis=2)
        cgrey = float(((cspread <= 24) & (np.abs(clevel - 128) <= 26)).mean())
        cwhite = float(((cspread <= 30) & (clevel > 200)).mean())
        if max(cgrey, cwhite) < 0.90:
            raise BurnInError(
                f"bit cell {i} is not a solid fill (grey {cgrey:.2f}, white "
                f"{cwhite:.2f}) -- plate mislocated, refusing to guess"
            )
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
