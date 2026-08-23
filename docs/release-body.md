## Trace v0.3.0-beta.7

**A dependency release. Nothing in the app changed on purpose — that is the point.** Trace now
builds on the current Qt and the current FFmpeg, and for the first time the build you download
uses the same toolchain as the build the timing was measured on. No feature was added, no
behaviour was intentionally altered, and the playback and scrub engines are untouched.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### What actually changed

| | before | now |
|---|---|---|
| Qt (in this ZIP) | 6.7.2 | **6.11.2** |
| Qt (on the dev box) | 6.10.2 | **6.11.2** |
| FFmpeg | 8.1.2 | **9.0.1** |

### Why the Qt version mattered more than it sounds

Every previous release ran a **different audio clock** than the build its timing was tuned on.
Qt rewrote its Windows audio sink in 6.9.1, changing what two of the values Trace uses as the
playback master clock actually mean — and the shipped ZIP was pinned below that line while
development ran above it. That gap is now closed in the only way that closes it: both are
6.11.2.

If you have ever felt that audio timing behaved differently in a downloaded build than
described, this is the release where that stops being possible.

### The window-drag audio dropout is NOT fixed, and now we know it is not a Qt problem

Holding the title bar still for a moment silences audio for about half a second. **It is
unchanged in this release and we are not claiming otherwise.**

What this release does buy is the elimination of a suspect. The fault has now been measured on
Qt 6.7.2, 6.10.2 and 6.11.2 and it is **the same on all three** — same size, same signature,
once per press rather than for as long as you hold. It is also not the renderer and not the
empty-state animation. And it is not really an audio bug: the whole application pauses for that
half second, audio is just the only part you can hear it in.

The cause is still unattributed. It is the next thing to look at, and it is deliberately not
being guessed at.

### What was checked before shipping this

Because a dependency swap can break things quietly, the regression was run against a control
build of the same code on the old toolchain:

- Decoded pixels are **bit-identical** between FFmpeg 8.1.2 and 9.0.1 across seven files,
  including 12-bit 4:4:4 with alpha, 10-bit ProRes and 10-bit HEVC.
- Full-pool scrub: **22 files, 88 gestures, every landing exact.**
- Playback cadence, including audio-mastered playback, flat.
- Every transport control, the menus, the accessibility tree, the empty state and the
  window chrome all retested on the new Qt.
- Decode throughput on large ProRes is unchanged to within measurement noise.

### Rollback knobs for this release

Unchanged from beta.6 — the same ones still apply.

| knob | effect |
|---|---|
| `TRACE_RENDERER=cpu` | the software renderer — first thing to try if the picture looks wrong |
| `TRACE_VOLUME_SLIDER=0` | mute-only button, no slider, no stored level |
| `TRACE_FS_MAG_FILTER=0` | fullscreen magnification back to the sharp sampler |
| `TRACE_MARK_ANIM=0` | empty-state mark held still |
| `TRACE_SCRUB_PAINT_GATE=0` | the beta.3 scrub paint gate off |

### Known and unchanged

- **The title-bar audio dropout, above.** Unchanged, understood better, not fixed.
- Multi-monitor setups with **different display scaling** were not re-tested on this Qt — Qt
  6.11 changed how that is handled and the test hardware is not currently connected. If you run
  two monitors at different scaling percentages and the window comes off a move the wrong
  shape, that is worth reporting.
- 8K ProRes 4444 XQ does not reach real time on this decoder and is a closed investigation, not
  a regression.
- EXR does not open: OpenImageIO is not in this build.
- HDR/PQ material gets the right matrix but no tonemap.
- Audio during scrubbing, reverse and off-speed playback is deliberately silent.

### If something is wrong

Help ▸ Report an Issue opens a pre-filled mail with the build identity in it. Press `H` to show
the diagnostics HUD and include a screenshot of it — nearly every question about playback,
scrubbing or audio is answered by that one line of text.
