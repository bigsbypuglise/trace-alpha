Trace — base UI icon assets
===========================
svg/       24x24 master grid. fill/stroke = currentColor, no baked colors.
           Strokes 1.75-2.0 px. Rounded terminals. Optical overshoot is intentional
           on circular glyphs (zoom, rotate, recent) - do not renormalize to bounds.
png/1x     24 px, white on transparent
png/2x     48 px, white on transparent

State opacity (apply to icon color, do not swap files):
  rest         0.82   (transport play/pause/shuttle: 0.92-1.00)
  hover        1.00   + control fill rgba(255,255,255,0.09)
  pressed      1.00   + control fill rgba(255,255,255,0.17), scale 0.96
  checked      1.00   + rgba(91,141,239,0.22) fill + 1px rgba(91,141,239,0.55) inset
                      + ui_check or ui_radio_selected glyph (never color alone)
  disabled     0.28
  unavailable  0.24   + tooltip stating why
  focus        1.00   + 2px #5B8DEF ring, 2px dark offset

Geometry: utility target 34x34 radius 8, glyph 20 px.
          play/pause target 44x44 radius 10, glyph 26 px.
          shuttle glyph 24 px in a 34x34 target.
Panel:    radius 14 (narrow 12), fill rgba(22,23,26,0.86), 1px rgba(255,255,255,0.10).

Notes:
- transport_scan_* are shuttle controls, not frame step. There is no frame-step icon
  in this set by design; Left/Right Arrow handle single frames.
- share_copy_lucidlink is a neutral chain glyph. It is not a LucidLink brand mark.
  Replace only with a licensed official asset.
- view_loop_off carries a slash so loop state is legible without color.
