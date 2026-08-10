Trace player UI — icon assets
=============================
svg/    24x24 grid (trace-play / trace-pause on a 1024 box), fill/stroke = currentColor.
        Tint via CSS color or XAML Foreground; no hardcoded colors.
png/1x  24 px    png/1.5x 36 px    png/2x  48 px    png/3x  72 px
        White (#FFFFFF) on transparent — apply opacity per state token.

State opacities (apply to icon color):
  rest       0.78
  hover      1.00   + control bg rgba(255,255,255,0.08)
  pressed    1.00   + control bg rgba(255,255,255,0.15)
  active     1.00   + control bg rgba(255,255,255,0.12) + 1px inner stroke 0.22
  disabled   0.26
  focus      1.00   + 2px #6AA6FF ring, 2px dark offset

Geometry: 44x44 hit target (compact 40x40), radius 10 (compact 9).
Center control: 60x60 radius 15 (compact 50x50 radius 13), mark 26 px (compact 22 px).
