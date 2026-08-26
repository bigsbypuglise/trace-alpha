# EXR + colour: the user-facing notes, ready to paste

> **THIS IS NOT A RELEASE BODY AND MUST NOT BE PUBLISHED AS ONE.**
>
> `docs/release-body.md` always describes the **current tag**. At the time of
> writing that tag is `v0.3.0-beta.8`, which is already published and whose
> notes correctly say *"EXR does not open: this build does not include
> OpenImageIO."* Editing that file to describe EXR would make a **published**
> release claim a feature it does not contain.
>
> So the EXR text lives here until the branch is merged and the next tag is
> cut. At that point, paste the two sections below into the new
> `docs/release-body.md` and delete this banner.
>
> Note also that this branch's copy of `docs/release-body.md` is **beta.7** --
> one release behind `main`, because the branch was cut before beta.8's body
> landed. Take `main`'s copy as the base when writing the next one; do not
> build on the copy in this branch.

---

## SECTION 1 -- for "What's new"

### EXR opens, with colour management and multilayer passes

Trace reads OpenEXR now, as a single still and as a numbered sequence, and it
reads it as **scene-linear float** rather than flattening to 8 bits on the way
in.

- **Colour transforms** through OpenColorIO: load a `.cube` LUT, or pick a
  config, input colour space, display and view from **View > Color
  Transform...**. `C` toggles the transform off and on so you can A/B against
  the raw image.
- **Multilayer passes.** `[` and `]` cycle the passes in a multilayer EXR, and
  **View > EXR Pass** lists them by name. Beauty, position, normal, depth and
  data passes each get an appropriate on-screen mapping, and the HUD always
  says which mapping is in force and what range it measured -- normalising is
  allowed, doing it silently is not.
- **Copy Current Frame now copies what is on screen**, not the raw source
  pixels. With a LUT or view transform active you get the graded picture. This
  is a deliberate change to existing behaviour for video too, and it is the
  only correct answer for EXR, whose source frame is scene-referred float with
  no single right 8-bit reading.

---

## SECTION 2 -- for "Known and unchanged"

Replace the existing `- EXR does not open: this build does not include
OpenImageIO.` line with the following.

- **Heavy multilayer EXR sequences do not play at full real time.** A
  27-channel multilayer DWAA sequence at 1920x1080 plays at roughly **15
  frames per second -- about two-thirds of real time** (measured 63.6-65.1% of
  24fps across four warm passes on the test system). **This is considered
  acceptable for review on this difficult file class, and it is not full
  real-time playback.** The limit is the file rather than anything schedulable:
  reading a single frame of that file takes about as long as the entire frame
  budget at 24fps, so the read alone is the whole budget before a pixel is
  drawn. Trace already issues almost exactly one read per frame shown, so there
  is no speculative work left to remove.
- **Single-layer EXR and PNG sequences are unaffected and hold real time** --
  measured 99.9% and 100.0% respectively, with no frames skipped.
- **The first play of any EXR sequence is roughly half speed** while Windows
  fills its file cache. Press Home and play it again; the second pass is the
  real figure. This is the operating system, not Trace.
- 10-bit output is still deliberately not in this build.
- HDR/PQ material gets the right colour matrix but no tonemap.

---

## SECTION 3 -- rollback knobs to add to that release's table

| knob | effect |
|---|---|
| `TRACE_SEQ_PREFETCH_STRIDE=0` | image-sequence prefetch back to the fixed one-frame-either-side window, if the new policy ever misbehaves |
| `TRACE_COLOR_LUT=<path>` | load a LUT at startup without the dialog |
| `TRACE_COLOR_VIEW=<config>[\|<input>\|<display>\|<view>]` | configure a display/view transform at startup |

---

## WHAT NOT TO SAY

- Do **not** describe the multilayer DWAA result as "real time", "smooth", or
  "fixed". It is a large measured improvement that still falls short of real
  time, and the notes should say both halves.
- Do **not** bury the limitation in a performance footnote. It is the headline
  limitation of the EXR milestone the way 8K ProRes 4444 XQ is the headline
  limitation of the video path, and `docs/release-notes-alpha.md` requires a
  measured gap to be stated **with its number**.
