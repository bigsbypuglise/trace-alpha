## Trace v0.3.0-beta.8

**A diagnostic release. No shipping behaviour changed on purpose.** This build closes out the
investigation into the title-bar freeze report with a real answer — not a fix, an answer — and
adds two file-based instruments for chasing it (or anything like it) in the field. Nothing about
playback, scrubbing, or audio was touched.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### The title-bar freeze: diagnosed, and it is not fixable without a much bigger change

Pressing and holding the real Windows title bar freezes the whole app — picture, HUD, audio,
everything — for about half a second before the window starts moving. This is now fully
understood:

**It is Windows itself, not Trace.** During the freeze the UI thread retrieves **no message of
any kind** — not input, not a timer, nothing. It happens *before* the window-move operation
even begins, so nothing inside that operation can be the cause. It reproduces identically on
three different Qt versions and with audio disabled entirely, which rules out both Qt and the
audio backend. The only thing that measurably works is intercepting the title-bar press and
starting the move ourselves — which moves the exact same freeze to a different Windows code
path and changes nothing.

**The only real fix would mean reimplementing the title bar by hand** — capture, hit-testing,
window positioning, and never handing the click back to Windows. That also means reimplementing
Snap, Aero Shake, edge magnetism, and multi-monitor drag behaviour ourselves, which is exactly
the tradeoff that was declined when the frameless-window idea was closed earlier — Trace keeps
the native title bar on purpose, for those. So this is closed as **understood, not fixed**, and
is not going to change without reopening that decision.

**What does work correctly**: when the freeze ends, audio and picture resume in sync — the
device was handed one buffer and then nothing for the whole freeze, and video follows the audio
clock rather than wall time, so playback picks back up 2–3 frames later, not twelve. That part
was already correct; it just took this investigation to confirm it.

### Two new diagnostics, both default off and free when off

Two file-based logs were added for chasing this and any future stall like it — file-based
because the dev HUD freezes right along with everything else during the fault, so it can't be
read at the moment that matters.

| env var | what it does |
|---|---|
| `TRACE_TICK_LOG=1` | writes one line per late playback tick to `%TEMP%\trace_tickstall.txt`, including audio counters that can see through a frozen UI thread |
| `TRACE_MSG_LOG=1` | writes a full Windows message-pump timeline to `%TEMP%\trace_msglog.txt`, so a stall shows up as a gap in the timeline rather than a guess |

Both are off by default and measured to cost nothing when off — confirmed against a control
build with hashed-identical DLLs, on both playback and with audio-mastered clips. Not something
you'd normally turn on; here in case a stall is ever worth chasing down again.

### Two experimental knobs, still default off, useful if you're testing something specific

Neither of these is new in this build, but neither has been called out plainly before. Both
default off; neither is recommended for normal use yet.

- **`TRACE_IO_READAHEAD=1`** — read-ahead buffering for remote storage (LucidLink and similar).
  It's correctness-verified — pixel-identical output against the plain path across forward and
  backward scrubbing — but it has **not been validated against a real remote mount**. Every
  performance figure behind it so far is a relative, synthetic on/off comparison on local media
  with an injected fake network delay, not a real cold LucidLink read. If you try it on a real
  `V:\` file, what I'd want to know is whether it visibly helps or hurts — not a number, a feel.
- **`TRACE_PLAYBACK_QUEUE=2`** — decodes up to N frames ahead of playback on a background
  worker instead of one at a time on the UI thread. Worth roughly +10% throughput on very heavy
  material (the 8K ProRes 4444 XQ plate). Depth 2 is the minimum that does anything; depth 1 is
  measured *worse* than off. Does nothing noticeable on ordinary 4K/1080p media, which already
  keeps up without it.

### Known and unchanged

- **8K ProRes 4444 XQ does not reach real-time playback**, and this is understood rather than
  an open bug: best measured is **13.64 fps (56.9% of real time)** at full quality with every
  frame shown, decode-bound at the CPU's own ceiling for that codec. No further work is planned
  here — a faster decoder or GPU decode would be needed, and GPU decode is explicitly out of
  scope. `TRACE_RT_DROP=0` is available if you want to compare against the frame-dropping
  fallback, but that fallback is not the answer and is not going to become the default.
- **A small window-position drift on multi-monitor setups with different display scaling**:
  going fullscreen and back (Escape) on a secondary monitor running at 150% scaling can land
  the window about 7 pixels higher than where it started. Size is unaffected — this is a small
  position nudge, not the framing bug to watch for. Known, not yet patched.
- 10-bit output is still deliberately not in this build — it needs a confirmed 10-bit display
  and a defined HDR/colour-management workflow before it's worth building, neither of which is
  in place yet.
- EXR does not open: this build does not include OpenImageIO.
- HDR/PQ material gets the right colour matrix but no tonemap.
- Audio during scrubbing, reverse playback, and off-speed (J/L) playback is deliberately silent.

### Rollback knobs for this release

Unchanged from beta.7 — the same ones still apply.

| knob | effect |
|---|---|
| `TRACE_RENDERER=cpu` | the software renderer — first thing to try if the picture looks wrong |
| `TRACE_VOLUME_SLIDER=0` | mute-only button, no slider, no stored level |
| `TRACE_FS_MAG_FILTER=0` | fullscreen magnification back to the sharp sampler |
| `TRACE_MARK_ANIM=0` | empty-state mark held still |
| `TRACE_SCRUB_PAINT_GATE=0` | the beta.3 scrub paint gate off |

### If something is wrong

Help ▸ Report an Issue opens a pre-filled mail with the build identity in it. Press `H` to show
the diagnostics HUD and include a screenshot of it — nearly every question about playback,
scrubbing or audio is answered by that one line of text.
