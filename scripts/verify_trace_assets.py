#!/usr/bin/env python3
"""Verify a Trace interface icon delivery against the repo's loading contract.

The application loads art by resource alias and size, so a delivery is a drop-in swap only
if the names and the pixel dimensions are exact. Those are precisely the things that are
invisible by eye -- a 25px export or a missing state looks fine in a folder listing and
fails at build or, worse, at runtime on one control.

Usage:
    python verify_trace_assets.py <path-to-assets-dir> [--app-icon] [--strict]

<path-to-assets-dir> is the directory that contains interface/ (and branding/ if the app
icon is in scope) -- i.e. the repo's assets/ folder, or a staging folder laid out the same
way.

    --app-icon   also verify assets/branding/app-icon/
    --strict     treat extra unexpected files in the interface folders as failures
                 rather than warnings

Exit code 0 means the delivery is a drop-in swap. Non-zero means it is not, and every
reason is printed.

Pillow is used for PNG dimensions when available; without it the script falls back to
reading the PNG IHDR header directly, so it has no hard dependency.
"""

from __future__ import annotations

import argparse
import struct
import sys
from pathlib import Path

# The shipping set. Folder -> glyph base names. These names ARE the contract: the resource
# alias is the same string for every entry except the two fullscreen ones, which the repo's
# .qrc remaps (fullscreen-enter -> fullscreen, fullscreen-exit -> exit-fullscreen). Keeping
# the disk names as written here is what preserves that mapping.
INTERFACE_SET = {
    "transport": ["play", "pause", "rewind", "fast-forward", "share"],
    "window": ["fullscreen-enter", "fullscreen-exit"],
    "common": ["copy-path", "show-in-explorer", "copy-lucidlink"],
}

PNG_SIZES = [24, 48]
OPTIONAL_PNG_SIZES = [72]  # allowed, never required

APP_ICON_PNGS = {
    "windows": [16, 24, 32, 48, 64, 256],
    "macos": [16, 32, 64, 128, 256, 512, 1024],
}
APP_ICON_OTHER = [
    "trace.ico",
    "trace.icns",
    "svg/trace-windows.svg",
    "svg/trace-macos.svg",
]

# Art for interaction states is a brightness multiplier applied at draw time, so variants
# would be unused and would disagree with the live rendering. Flagging them early saves the
# designer the work rather than catching it in review.
STATE_SUFFIXES = ["-hover", "-pressed", "-disabled", "-active", "-focus", "-normal"]


def png_size(path: Path) -> tuple[int, int] | None:
    """Return (width, height) for a PNG, or None if it cannot be read."""
    try:
        from PIL import Image  # type: ignore

        with Image.open(path) as im:
            return im.size
    except ImportError:
        pass
    except Exception:
        return None

    # Fallback: parse the IHDR chunk. A PNG is an 8-byte signature, then a 4-byte length,
    # then "IHDR", then width and height as big-endian uint32.
    try:
        with path.open("rb") as f:
            head = f.read(24)
        if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
            return None
        return struct.unpack(">II", head[16:24])
    except Exception:
        return None


def check_interface(root: Path, strict: bool) -> tuple[list[str], list[str]]:
    errors: list[str] = []
    warnings: list[str] = []
    iface = root / "interface"

    if not iface.is_dir():
        return ([f"missing directory: {iface}"], warnings)

    for folder, names in INTERFACE_SET.items():
        d = iface / folder
        if not d.is_dir():
            errors.append(f"missing directory: {d}")
            continue

        expected: set[str] = set()
        for name in names:
            svg = d / f"{name}.svg"
            expected.add(svg.name)
            if not svg.is_file():
                errors.append(f"missing master: {folder}/{name}.svg")

            for size in PNG_SIZES:
                png = d / f"{name}-{size}.png"
                expected.add(png.name)
                if not png.is_file():
                    errors.append(f"missing rendition: {folder}/{name}-{size}.png")
                    continue
                dims = png_size(png)
                if dims is None:
                    errors.append(f"unreadable PNG: {folder}/{name}-{size}.png")
                elif dims != (size, size):
                    errors.append(
                        f"wrong size: {folder}/{name}-{size}.png is "
                        f"{dims[0]}x{dims[1]}, expected {size}x{size}"
                    )

            for size in OPTIONAL_PNG_SIZES:
                expected.add(f"{name}-{size}.png")

        for f in sorted(d.iterdir()):
            if not f.is_file() or f.name == ".gitkeep":
                continue
            if f.name in expected:
                continue
            stem = f.stem
            if any(stem.endswith(s) for s in STATE_SUFFIXES):
                errors.append(
                    f"interaction-state art is not used: {folder}/{f.name} — hover is a "
                    f"1.35x brightness multiply and pressed is 0.72x, applied at draw time"
                )
            elif strict:
                errors.append(f"unexpected file: {folder}/{f.name}")
            else:
                warnings.append(
                    f"unexpected file (not embedded, so it will be ignored): "
                    f"{folder}/{f.name}"
                )

    return errors, warnings


def check_app_icon(root: Path) -> list[str]:
    errors: list[str] = []
    base = root / "branding" / "app-icon"
    if not base.is_dir():
        return [f"missing directory: {base}"]

    for rel in APP_ICON_OTHER:
        if not (base / rel).is_file():
            errors.append(f"missing app icon file: branding/app-icon/{rel}")

    for platform, sizes in APP_ICON_PNGS.items():
        for size in sizes:
            p = base / "png" / platform / f"trace-{size}.png"
            if not p.is_file():
                errors.append(f"missing app icon PNG: png/{platform}/trace-{size}.png")
                continue
            dims = png_size(p)
            if dims is None:
                errors.append(f"unreadable PNG: png/{platform}/trace-{size}.png")
            elif dims != (size, size):
                errors.append(
                    f"wrong size: png/{platform}/trace-{size}.png is "
                    f"{dims[0]}x{dims[1]}, expected {size}x{size}"
                )
    return errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("assets_dir", help="the assets/ directory (contains interface/)")
    ap.add_argument("--app-icon", action="store_true", help="also verify branding/app-icon/")
    ap.add_argument(
        "--strict",
        action="store_true",
        help="treat unexpected files in interface folders as failures",
    )
    args = ap.parse_args()

    root = Path(args.assets_dir).expanduser().resolve()
    if not root.is_dir():
        print(f"FAIL  not a directory: {root}")
        return 2

    errors, warnings = check_interface(root, args.strict)
    if args.app_icon:
        errors += check_app_icon(root)

    expected_files = sum(len(v) for v in INTERFACE_SET.values()) * 3
    print(f"Trace interface asset check — {root}")
    print(f"  interface set: {expected_files} files expected "
          f"({sum(len(v) for v in INTERFACE_SET.values())} glyphs x svg + 24px + 48px)")
    if args.app_icon:
        print("  app icon: included")
    print()

    for w in warnings:
        print(f"WARN  {w}")
    for e in errors:
        print(f"FAIL  {e}")

    print()
    if errors:
        print(f"{len(errors)} problem(s). This delivery is NOT a drop-in swap yet.")
        return 1

    if warnings:
        print(f"Complete, with {len(warnings)} warning(s). Safe to swap.")
    else:
        print("Complete. This is a drop-in swap: no .qrc or code change needed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
