#!/usr/bin/env python3
"""Verify a Trace interface icon delivery against the repo's loading contract.

The application loads art by resource alias and size, so a delivery is a drop-in swap only
if the names and the pixel dimensions are exact. Those are precisely the things that are
invisible by eye -- a 25px export or a missing state looks fine in a folder listing and
fails at build or, worse, at runtime on one control.

THE EXPECTED SET IS DERIVED, NEVER WRITTEN DOWN HERE. It is read from the two files that
actually decide what the build loads:

    app/resources.qrc   every <file> entry, resolved relative to the .qrc itself
    app/trace.rc        the Windows resource compiler's ICON line

An earlier version of this script hard-coded the list. That made it a duplicate of the
contract rather than a reading of it, so a future .qrc edit would leave it stale -- and a
stale instrument accusing a correct build is the failure this project has recorded ten
times. It also meant the fullscreen alias remap (fullscreen-enter -> fullscreen) had to be
described in a comment; deriving from the file PATHS makes it disappear, because the path
is the disk name.

trace.rc is here because it is a second, separate contract: assets/ was reorganised once
and the .qrc was re-pointed while trace.rc was not, so the Windows resource compiler
dangled on a path no count of .qrc entries could have found.

WHAT THIS CATCHES THAT THE BUILD DOES NOT. rcc already fails on a missing entry, so that
half is covered twice over. A 25px export named -24 is not: it builds green and renders
wrong on one control at runtime.

Usage:
    python scripts/verify_trace_assets.py [assets-dir] [--app-icon] [--strict] [--no-pillow]

With no arguments it checks the repo's own assets/ against the repo's own contract, which
is what CI runs. Pass a directory to check a staging folder laid out the same way.

    --app-icon    also check the app-icon files that are NOT embedded (macOS renditions,
                  the extra Windows sizes, the .icns and the SVG masters). These are a
                  delivery convention, not a build contract -- nothing reads them at
                  build time -- so they are listed here and are off by default.
    --strict      unexpected files in the interface folders are failures, not warnings.
                  The .qrc's own comment states that assets/interface/ carries the SVG
                  masters plus exactly the PNGs it embeds and nothing else, so this is
                  the repo's stated invariant and CI asserts it.
    --no-pillow   ignore Pillow and read PNG headers directly. CI runners generally have
                  no Pillow, so this is how the fallback path gets exercised deliberately
                  rather than only on machines that happen to lack it.

Exit 0 means the delivery is a drop-in swap. 1 means it is not, and every reason is
printed. 2 means the check could not be run at all -- a missing contract file or assets
directory, which must never be mistaken for a pass.
"""

from __future__ import annotations

import argparse
import re
import struct
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

# Art for interaction states is a brightness multiplier applied at draw time (hover 1.35x,
# pressed 0.72x), so a delivered variant would be embedded-and-unused and would disagree
# with the live rendering. Flagging it here saves the designer the work rather than
# catching it in review.
STATE_SUFFIXES = ["-hover", "-pressed", "-disabled", "-active", "-focus", "-normal"]

# App-icon files that no build step reads. Kept as a delivery-completeness list behind
# --app-icon and deliberately NOT presented as derived, because there is nothing to derive
# them from: the .qrc embeds four Windows PNGs and trace.rc names the .ico, and everything
# below that is convention.
UNEMBEDDED_APP_ICON_PNGS = {
    "windows": [24, 64],
    "macos": [16, 32, 64, 128, 256, 512, 1024],
}
UNEMBEDDED_APP_ICON_OTHER = [
    "trace.icns",
    "svg/trace-windows.svg",
    "svg/trace-macos.svg",
]

SIZE_SUFFIX = re.compile(r"^(?P<base>.+)-(?P<size>\d+)$")


def split_size(stem: str) -> tuple[str, int | None]:
    """'play-24' -> ('play', 24); 'trace' -> ('trace', None).

    The trailing integer is the size the FILENAME CLAIMS. Comparing it against the pixels
    is the whole point of this script, so the two must be read from different places.
    """
    m = SIZE_SUFFIX.match(stem)
    if not m:
        return stem, None
    return m.group("base"), int(m.group("size"))


def png_size(path: Path, use_pillow: bool = True) -> tuple[int, int] | None:
    """Return (width, height) for a PNG, or None if it cannot be read."""
    if use_pillow:
        try:
            from PIL import Image  # type: ignore

            with Image.open(path) as im:
                return im.size
        except ImportError:
            pass
        except Exception:
            return None

    # Fallback: parse the IHDR chunk. A PNG is an 8-byte signature, then a 4-byte length,
    # then "IHDR", then width and height as big-endian uint32. This is the path CI takes.
    try:
        with path.open("rb") as f:
            head = f.read(24)
        if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
            return None
        return struct.unpack(">II", head[16:24])
    except Exception:
        return None


def read_qrc(qrc: Path) -> list[Path]:
    """Absolute paths of every <file> the .qrc embeds, resolved relative to the .qrc."""
    tree = ET.parse(qrc)
    out: list[Path] = []
    for node in tree.getroot().iter("file"):
        text = (node.text or "").strip()
        if text:
            out.append((qrc.parent / text).resolve())
    return out


def read_rc(rc: Path) -> list[Path]:
    """Absolute paths of every ICON the Windows resource script references."""
    out: list[Path] = []
    for line in rc.read_text(encoding="utf-8", errors="replace").splitlines():
        m = re.search(r'\bICON\s+"([^"]+)"', line)
        if m:
            out.append((rc.parent / m.group(1)).resolve())
    return out


def derive_contract(contract_root: Path) -> tuple[list[Path], list[str]]:
    """Every asset path the build loads, from the files that decide it."""
    problems: list[str] = []
    required: list[Path] = []

    qrc = contract_root / "app" / "resources.qrc"
    rc = contract_root / "app" / "trace.rc"

    if not qrc.is_file():
        problems.append(f"contract file not found: {qrc}")
    else:
        try:
            required += read_qrc(qrc)
        except ET.ParseError as e:
            problems.append(f"{qrc} is not valid XML: {e}")

    if not rc.is_file():
        problems.append(f"contract file not found: {rc}")
    else:
        required += read_rc(rc)

    return required, problems


def check(
    delivery: Path,
    repo_assets: Path,
    required: list[Path],
    strict: bool,
    use_pillow: bool,
) -> tuple[list[str], list[str], int]:
    errors: list[str] = []
    warnings: list[str] = []

    # Map each required path onto the delivery being checked. Everything embedded lives
    # under assets/; anything that does not is reported rather than silently skipped,
    # because it would mean the contract grew a shape this script does not understand.
    wanted: dict[Path, Path] = {}   # delivery path -> repo-relative path
    for abs_path in required:
        try:
            rel = abs_path.relative_to(repo_assets)
        except ValueError:
            warnings.append(
                f"embedded file is outside assets/ and was not checked: {abs_path}"
            )
            continue
        wanted[(delivery / rel)] = rel

    # Every embedded PNG in interface/ has an SVG master beside it. The .qrc cannot state
    # this -- it embeds no SVGs, because Trace does not link Qt6::Svg -- but its own
    # comment does, and the masters are what a redesign starts from.
    masters: dict[Path, Path] = {}
    for target, rel in list(wanted.items()):
        if rel.parts[0] != "interface" or target.suffix.lower() != ".png":
            continue
        base, _ = split_size(target.stem)
        svg_rel = rel.parent / f"{base}.svg"
        masters[delivery / svg_rel] = svg_rel

    for target, rel in sorted({**wanted, **masters}.items()):
        embedded = target in wanted
        if not target.is_file():
            kind = "embedded file" if embedded else "SVG master"
            errors.append(f"missing {kind}: {rel.as_posix()}")
            continue
        if target.suffix.lower() != ".png":
            continue
        _, claimed = split_size(target.stem)
        if claimed is None:
            continue
        dims = png_size(target, use_pillow)
        if dims is None:
            errors.append(f"unreadable PNG: {rel.as_posix()}")
        elif dims != (claimed, claimed):
            errors.append(
                f"wrong size: {rel.as_posix()} is {dims[0]}x{dims[1]}, "
                f"but its name claims {claimed}x{claimed}"
            )

    # Nothing extra in the interface working copies. Checked only in folders the contract
    # actually reaches, so an unrelated directory elsewhere in a staging tree is not
    # anyone's business here.
    expected_names: dict[Path, set[str]] = {}
    for target in {**wanted, **masters}:
        if "interface" in target.parts:
            expected_names.setdefault(target.parent, set()).add(target.name)

    for folder, names in sorted(expected_names.items()):
        if not folder.is_dir():
            continue
        for f in sorted(folder.iterdir()):
            if not f.is_file() or f.name == ".gitkeep" or f.name in names:
                continue
            # Strip the size before testing the state suffix, so play-hover-24.png is
            # caught as well as play-hover.png. The earlier version tested the whole stem
            # and only warned on the sized form.
            base, _ = split_size(f.stem)
            shown = f"{folder.name}/{f.name}"
            if any(base.endswith(s) for s in STATE_SUFFIXES):
                errors.append(
                    f"interaction-state art is not used: {shown} - hover is a 1.35x "
                    f"brightness multiply and pressed is 0.72x, applied at draw time"
                )
            elif strict:
                errors.append(f"unexpected file (nothing embeds it): {shown}")
            else:
                warnings.append(
                    f"unexpected file (not embedded, so it will be ignored): {shown}"
                )

    return errors, warnings, len(wanted)


def check_unembedded_app_icon(delivery: Path, use_pillow: bool) -> list[str]:
    errors: list[str] = []
    base = delivery / "branding" / "app-icon"
    if not base.is_dir():
        return [f"missing directory: branding/app-icon"]

    for rel in UNEMBEDDED_APP_ICON_OTHER:
        if not (base / rel).is_file():
            errors.append(f"missing app icon file: branding/app-icon/{rel}")

    for platform, sizes in UNEMBEDDED_APP_ICON_PNGS.items():
        for size in sizes:
            p = base / "png" / platform / f"trace-{size}.png"
            rel = f"branding/app-icon/png/{platform}/trace-{size}.png"
            if not p.is_file():
                errors.append(f"missing app icon PNG: {rel}")
                continue
            dims = png_size(p, use_pillow)
            if dims is None:
                errors.append(f"unreadable PNG: {rel}")
            elif dims != (size, size):
                errors.append(
                    f"wrong size: {rel} is {dims[0]}x{dims[1]}, "
                    f"but its name claims {size}x{size}"
                )
    return errors


def main() -> int:
    here = Path(__file__).resolve().parent
    default_root = here.parent

    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument(
        "assets_dir",
        nargs="?",
        help="the assets/ directory to check (default: the repo's own)",
    )
    ap.add_argument(
        "--contract-root",
        default=str(default_root),
        help="repo root holding app/resources.qrc and app/trace.rc",
    )
    ap.add_argument(
        "--app-icon",
        action="store_true",
        help="also check the app-icon files that nothing embeds",
    )
    ap.add_argument(
        "--strict",
        action="store_true",
        help="unexpected files in the interface folders are failures",
    )
    ap.add_argument(
        "--no-pillow",
        action="store_true",
        help="read PNG headers directly, ignoring Pillow",
    )
    args = ap.parse_args()

    contract_root = Path(args.contract_root).expanduser().resolve()
    repo_assets = contract_root / "assets"
    delivery = (
        Path(args.assets_dir).expanduser().resolve() if args.assets_dir else repo_assets
    )

    if not delivery.is_dir():
        print(f"FAIL  not a directory: {delivery}")
        return 2

    required, problems = derive_contract(contract_root)
    if problems:
        for p in problems:
            print(f"FAIL  {p}")
        print("\nThe contract could not be read, so nothing was verified.")
        return 2
    if not required:
        print("FAIL  the contract files name no assets at all - refusing to pass.")
        return 2

    errors, warnings, embedded_count = check(
        delivery, repo_assets, required, args.strict, not args.no_pillow
    )
    if args.app_icon:
        errors += check_unembedded_app_icon(delivery, not args.no_pillow)

    reader = "PNG header" if args.no_pillow else "Pillow (or PNG header)"
    print(f"Trace asset check - {delivery}")
    print(f"  contract:  {(contract_root / 'app' / 'resources.qrc')}")
    print(f"             {(contract_root / 'app' / 'trace.rc')}")
    print(f"  derived:   {embedded_count} embedded files, plus their SVG masters")
    if args.app_icon:
        print("  app icon:  unembedded delivery set included")
    if args.strict:
        print("  strict:    unexpected files are failures")
    print(f"  sizes via: {reader}")
    print()

    for w in warnings:
        print(f"WARN  {w}")
    for e in errors:
        print(f"FAIL  {e}")

    print()
    if errors:
        print(f"{len(errors)} problem(s). This delivery is NOT a drop-in swap.")
        return 1

    if warnings:
        print(f"Complete, with {len(warnings)} warning(s). Safe to swap.")
    else:
        print("Complete. This is a drop-in swap: no .qrc or code change needed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
