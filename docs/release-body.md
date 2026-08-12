## Trace v0.2.0-alpha.1

The interface pass. Trace has had a real transport, real menus and a real window model since
the last alpha — but it is **still alpha**, deliberately, and the section below says exactly
what is missing.

Windows, portable ZIP, x64. Unzip anywhere and run `Trace.exe`. There is no installer by
design (see *Packaging* below).

### What's new

**A floating transport over the video.** Play/Pause, Rewind, Fast-forward, a timeline and a
Share menu, drawn by the renderer and composited over the picture. It reveals on pointer
movement and fades after two seconds of inactivity; in fullscreen the cursor hides with it.

**Rewind and Fast-forward are shuttle controls, not frame steps.** Each press advances one
stage — 2× → 5× → 10× → 30×, in either direction — and a press in the opposite direction turns
around at 2×. Play returns to normal speed, Pause clears the rate. Shuttle playback is silent
and your mute setting is restored when normal playback resumes. **Frame stepping is now
keyboard-only**: Left and Right arrows, still exactly one frame, still exact.

**The window is the shape of the media.** Open a 4:5 or 9:16 or square file and the window
adopts its display aspect ratio, so the picture touches all four edges with no black bars.
Pixel aspect ratio and rotation metadata are read from the file, not assumed from width ÷
height. Resizing keeps the ratio (View ▸ Lock Window to Media Aspect Ratio, on by default;
turn it off to resize freely). 4K files open at a sensible size rather than filling the
desktop.

**View scaling.** Actual Size (Ctrl+0), Fit to Window, Zoom In/Out (Ctrl+ +/−), and drag to pan
when the picture is larger than the viewport. **Above 1:1 the picture is point-sampled** — at
4:1 you are looking at actual pixels, which is the point. Note this includes chroma, so on
4:2:0 material a colour sample really does cover a 2×2 block of luma samples; that is what the
file contains.

**A Movie Inspector** (Ctrl+I) that says where every value came from — `encoded` (what the file
states), `file` (the file on disk), `observed` (this window right now), `playback` (what Trace
did about it). Untagged colour metadata is reported as untagged, with Trace's inference shown
separately rather than presented as the file's own claim.

**Time display** with four modes: Frame Count, Seconds, Elapsed, and **SMPTE source timecode**.
SMPTE reads the timecode embedded in the file, including drop-frame, and is **unavailable
rather than invented** when the file carries none. Go to Frame (Ctrl+G) and Go to Timecode
(Ctrl+Shift+G) validate and refuse rather than clamping.

**Share menu** — Copy File Path, Show in File Explorer, and **Copy LucidLink Link**, which asks
the installed LucidLink integration for the link rather than constructing one from the path.

**Also**: Open Recent with Clear Recent Files · rotate/flip view transforms (Edit menu, temporary,
never written to the file) · Loop · 0.5× playback · Copy Current Frame (Ctrl+C) · Always on Top ·
Keyboard Shortcuts window · Trace Help · zero-based frame numbering throughout, so an N-frame
file ends at N−1.

**Accessibility**: the composited transport is exposed to Windows UI Automation and has been
driven with Narrator — Play/Pause, Rewind, Fast-forward, Timeline and Share are announced and
activate with Narrator+Enter. Navigate it with the **Narrator cursor**, not Tab: the transport
is deliberately outside the tab chain so it cannot steal the Space bar from playback.

### Known gaps — please read before filing

These are known. Reports about them are not needed; reports about anything else are very
welcome.

- **EXR does not open.** OpenImageIO is not built into this package, so `TRACE_WITH_OIIO` is
  undefined in both vcpkg and CI. EXR and OCIO display transforms are future work.
- **HDR and BT.2020 content will look wrong.** The correct matrix is applied but there is **no
  tonemap**, on either renderer. PQ/HLG material is not usable in this build.
- **10-bit display output is not supported.** Output is 8-bit SDR. This is *not* the same thing
  as high-bit-depth processing, which does work — 10-bit and 12-bit sources are decoded and
  converted at their real bit depth; it is the final output that is 8-bit.
- **Mixed-monitor DPI is unvalidated.** The window-shaping system is DPI-aware by construction
  and passes a synthetic test across 100/125/150/200%, but it has **never run on a real
  multi-monitor setup** — no monitor-to-monitor move, no live DPI change, no fullscreen on a
  secondary display. If you work across two screens at different scaling, expect this to be the
  rough edge, and please report what you see. **This is the main reason this release is still
  alpha rather than beta.**
- **LucidLink cold reads deliver roughly 600–800 Mbps.** Files above that bitrate will not play
  in real time from a cold cache no matter how they are buffered — a 4.5 Gbps plate cannot
  stream. Warm playback is normal. 4K ProRes 422/HQ-class media is the realistic target.
- **Copy LucidLink Link needs the LucidLink Windows shell extension installed**, and it
  identifies the command by its English display text. On a localized Windows it will report the
  integration as unavailable rather than invoke the wrong menu item — that is deliberate.

### If the picture looks wrong

Set `TRACE_RENDERER=cpu` and relaunch. That selects the CPU renderer instead of the D3D11
default and is the first thing to try for any picture problem — it is slower and slightly
softer, but it is the reference path. Tell us if it fixes what you were seeing; that is the
single most useful thing a report can contain.

### Packaging

Portable ZIP only, by deliberate choice — no installer, no file associations, no auto-updater,
and nothing written to the registry. Delete the folder and Trace is gone. An installer comes
when packaging and playback are stable.

If you place a writable `trace.ini` next to `Trace.exe`, Trace uses it for settings and stays
fully portable. Trace never creates that file itself; its presence is how you ask for portable
mode.

---
