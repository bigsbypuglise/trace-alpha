## Trace v0.3.0-beta.6

**Sound, and the small things you asked about.** Trace now opens audio files, has a
volume slider, filters the fullscreen picture when it enlarges it, animates the empty-state
mark, and no longer lets a bare `F` open the File menu. Nineteen commits past
`v0.3.0-beta.5`; the playback and scrub engines are unchanged and measure flat.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by design.

### Audio files open and the transport drives them

`wav` `mp3` `m4a` `aac` `flac` `ogg` `opus`. Play, pause, scrub, Go To, Home/End and Loop all
work; the window keeps the prism mark on screen because there is no picture to show. The
readout defaults to **Elapsed**, never Frame Count — an audio file has no frames, so the frame
index you can still see is Trace's own synthetic one at a nominal 24fps, and the HUD and the
Movie Inspector both say so rather than presenting it as a property of your file.

Dragging is silent by design; the release seeks and resumes. That follows the existing rule
that sound is 1× forward playback only.

### A volume slider

Hover or click the speaker and a slider slides out between Mute and Loop; it collapses on its
own after a moment. The scroll wheel over the speaker adjusts in 5% steps. **Your level now
persists between sessions** (written only at settled values — the end of a drag, a wheel step
— never continuously while you drag).

Volume is a gain on the audio device and never touches the playback clock, so it cannot affect
timing. `TRACE_VOLUME_SLIDER=0` restores the previous mute-only button exactly, including not
leaving a stored level silently in force with no control to show it.

### Fullscreen no longer aliases

Reported by a tester and confirmed: pressing F11 on material **smaller than your screen** made
Trace enlarge it with a point sampler, which shows as hard, stair-stepped edges. That sampler
is deliberate for deliberate zoom — someone at 4:1 is inspecting samples — but a fullscreen fit
is not that, and it is why the report was hard to reproduce on an ultrawide, where 4K material
is being *reduced* in fullscreen and never hits the case.

**The fullscreen fit now filters when it magnifies. Actual Size and Zoom In keep the sharp
sampler, fullscreen or not.** The windowed fit still takes the point sampler when it magnifies;
that is the decision's stated width, not an oversight. `TRACE_FS_MAG_FILTER=0` is the rollback.

### The empty-state mark animates

With nothing open, the prism mark's edge gradient rotates and its glow cycles through its
colour ladder over an 18-second loop — the design package's own animation, which no still image
could carry. It runs only when there is no picture on screen and the window is in front, so
opening a video stops it by construction and a background window costs nothing.
`TRACE_MARK_ANIM=0` holds it still.

### A bare letter belongs to Trace again

If you had used the menus at all, `F` would open the File menu instead of changing the time
readout, and `E` would open Edit. The menu bar was keeping keyboard focus after you left a menu
— invisibly, because the top strip fades away — and in that state Windows treats a bare letter
as a menu shortcut.

**`F`, `S`, `E` and `T` now always change the time display, and `H` always toggles the
diagnostics HUD.** `Alt`+`F`, `Alt`+`E` and the rest still open their menus, including when the
strip is hidden, and arrow-key navigation between open menus is unchanged. **`Space` also
toggles playback again after you have used a menu**, which was the same fault seen from the
other side.

### Also

- The Playback Speed menu follows the engine through pause and play, instead of leaving `0.5×`
  ticked over a paused file that will next play at `1×`.
- The Loop button no longer announces a persistence that was removed — Loop still starts off
  each session and survives a file change within one.

### Rollback knobs for this release

| knob | effect |
|---|---|
| `TRACE_VOLUME_SLIDER=0` | mute-only button, no slider, no stored level |
| `TRACE_FS_MAG_FILTER=0` | fullscreen magnification back to the sharp sampler |
| `TRACE_MARK_ANIM=0` | empty-state mark held still |
| `TRACE_SCRUB_PAINT_GATE=0` | the beta.3 scrub paint gate off |
| `TRACE_RENDERER=cpu` | the software renderer — first thing to try if the picture looks wrong |

### Known and unchanged

- 8K ProRes 4444 XQ does not reach real time on this decoder and is a closed investigation, not
  a regression.
- EXR does not open: OpenImageIO is not in this build.
- HDR/PQ material gets the right matrix but no tonemap.
- Audio during scrubbing, reverse and off-speed playback is deliberately silent.

### If something is wrong

Help ▸ Report an Issue opens a pre-filled mail with the build identity in it. Press `H` to show
the diagnostics HUD and include a screenshot of it — nearly every question about playback,
scrubbing or audio is answered by that one line of text.
