# Trace assets

Two kinds of thing live here, and the difference is the point of the layout:
**`source/` is a master you never edit, and everything else is a working copy
named for what it does in the app.**

```
assets/
├── branding/
│   └── app-icon/       trace.ico, trace.icns, the png/{windows,macos} sets
│                       and the two svg masters
├── interface/
│   ├── transport/      play, pause, rewind, fast-forward, prev-frame, next-frame
│   ├── window/         fullscreen-enter, fullscreen-exit
│   └── common/         empty — volume, share, inspector, zoom, rotate and loop
│                       arrive here when those features are real
├── source/
│   └── original-design-package/    the complete untouched export
└── README.md
```

## The rule

`source/original-design-package/` is the **untouched master**: the design
package exactly as it was delivered, including the parts Trace does not use and
the parts it never will. Nothing is renamed, resized or removed there, and
nothing outside it is authoritative. If a glyph is wrong, it is wrong in a
working copy and the master is what you re-copy from.

Everything under `branding/` and `interface/` is a **working copy, named for
its behaviour**. That naming is not cosmetic — it is the same rule spec phase 2
applied to the artwork itself: a control's icon has to mean what the control
does. The package's `transport_scan_reverse` / `transport_scan_forward` are
`rewind` / `fast-forward` here, because that is the job they are drawn for.

## What is in a working copy, and what is not

`interface/` carries, for each glyph, **the SVG master plus exactly the PNG
renditions `app/resources.qrc` embeds** — nothing else. So the directory
listing and the `.qrc` agree by construction, and a rendition sitting in
`interface/` that no `.qrc` line mentions is a mistake you can see.

Two consequences worth stating, because both have bitten before:

- **The app embeds PNG and does not link `Qt6::Svg`.** The SVGs here are
  masters to re-export from, not something the build reads. Moving Trace to
  vector icons is a reasonable change but it is a real decision with a
  deployment consequence; it must not happen as a side effect of a folder move.
- **The package ships 24px @1x and 48px @2x and has no @3x.** A 3x display
  scales the 48px master. The two frame-step glyphs are the exception and do
  have a 72px rendition — see below.

## The two frame-step glyphs

`prev-frame` and `next-frame` come from the **superseded first-pass set**, which
the approved package keeps as `player-icons/` and labels "SUPERSEDED — kept for
reference". Using them is deliberate. The approved `base-ui-icons/` set has no
frame-step glyph by design, and its `transport_scan_*` pair is the artwork for
the *redesigned* Rewind and Fast-forward — but those two buttons still perform
single-frame stepping until spec phases 4 and 5. Shipping scan artwork over
stepping behaviour would put a lie on a visible control, so the artwork moves
when the behaviour does, and these two leave `interface/transport/` then.

## Adding a glyph

1. Copy it out of `source/original-design-package/`, never out of another
   working copy.
2. Name it for the control it drives, not for the file it came from.
3. Add the `.qrc` line in the same change, or the working copy and the build
   stop agreeing — which is exactly the state this layout was created to fix.
