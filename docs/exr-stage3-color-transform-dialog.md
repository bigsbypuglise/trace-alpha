# EXR/colour stage 3: the Color Transform dialog, and what it costs

Record of what was built and measured. 2026-08-24, branch
`exr-stage0-dependencies`, physical panel 5120x1440 @ 239.999Hz. Commits
`0e7d7c6` (config discovery) · `d16a98e` (the dialog) · `84a9f3f` (harness) ·
`0841201` (the resolved-config fix and `TRACE_COLOR_VIEW`).

**NOT merged.** Step 1's record is `docs/exr-stage3-cadence-instrument.md`; read
it first, because every figure here is measured against its baseline.

---

## THE HEADLINE, STATED FIRST BECAUSE IT IS A STOP

**EXR sequence playback does NOT hold rate with an ACES view transform active,
and it is not close.** On the file step 1 identified as the sensitive one:

| `R2_OP_Stacks_01`, 217 frames, 1920x1080, PIZ | presented | skip | handler>budget | handler max |
|---|---|---|---|---|
| no transform (step 1's baseline, re-run) | **99.9%** | 0 | 0 of 215 | 37.8 ms |
| **ACES 2.0 display transform** | **36.4%** | **138** | **79 of 79** | **129.5 ms** |
| a `.cube` LUT, same file | **100.0%** | 0 | **0 of 215** | **28.4 ms** |

Per the instruction, nothing was optimised. What follows is the measurement and
the one isolating experiment that says where the cost is.

---

## THE COST IS THE ACES OP CHAIN, NOT THE PLUMBING, AND THE LUT ROW PROVES IT

The third row is the decisive one and it was run for exactly this reason. **A
LUT on the same file, through the same float path, the same parallel row bands
and the same display stage, reads 100.0% of real time with `handler>budget 0 of
215`** -- and its handler max of **28.4 ms is LOWER than the 37.8 ms with no
transform at all**, because the OCIO branch skips `measureFloatRange()` (a full
extra pass over the frame purely to fill in the HUD's clipping figure) and
replaces the `Gamma22` mapping's 255-threshold search.

So the float buffer, the descriptors, the band split, the frame handling and the
re-delivery are all free. **What costs ~92 ms per 1080p frame is the ACES 2.0
display transform itself.**

**A STAGE 1 PREMISE EXPIRED HERE AND IT IS WORTH NAMING.** Stage 1 recorded the
colour stage at **9.3 ns/pixel, linear in pixel count**, which at 1920x1080 is
~19 ms single-threaded and 3-4 ms in parallel bands -- and step 1 predicted, from
that figure, that the transform might cost nothing net. **That measurement was
taken on a `.cube` LUT**, which is one interpolated 3D lookup. An ACES 2.0
DisplayViewTransform is a large analytic op chain. Measured here it is
**~45 ns/pixel in parallel bands, roughly five times the recorded rate**, and the
LUT row reproduces the old figure on the same file in the same session -- so the
two numbers are both right and describe different transforms. **Do not quote
9.3 ns/pixel for a display transform.**

The multilayer file moves the same way and is included for completeness, but it
cannot decide anything: it was already 4.6x over budget before the transform.

| `MultlayerAces`, 97 frames, 27ch, DWAA | presented | skip | handler>budget |
|---|---|---|---|
| no transform | 29.4% | 68 | 28 of 28 (max 190.5) |
| ACES 2.0 display transform | 20.6% | 78 | 20 of 20 (max 231.6) |

---

## WHAT WAS BUILT

### Config discovery: three sources, one resolver, and it removes a live trap

The brief asked for an explicit file, `$OCIO`, and OCIO's built-in `ocio://`
configs so Trace works with nothing installed.

```
an explicit file      -> CreateFromFile(path)        source File
an "ocio://..." URI   -> CreateFromFile(uri)         source Builtin
nothing, $OCIO set    -> CreateFromFile($OCIO)       source Env
nothing, $OCIO unset  -> CreateFromFile(ocio://default)  source Builtin
```

One resolver, shared by the dialog's enumerators, `setConfig()` and the selftest,
so the config named in the combo box and the config the processor is built from
cannot be different things.

**`OCIO::Config::CreateFromEnv()` IS NOW USED NOWHERE, AND THAT IS A FIX RATHER
THAN A STYLE CHOICE.** Stage 1's DisplayView branch called it whenever
`configPath` was empty. Measured with the new `scripts/measure/ocioprobe`: with
`$OCIO` unset, `CreateFromEnv()` **neither throws nor returns null** -- it returns
a *"Color management disabled"* RAW config carrying **one colour space and one
display**, and announces that only on stderr, which no GUI ever shows. An empty
path would therefore have compiled a transform that looks loaded and does
nothing.

Other things the probe settled rather than assumed: this OCIO carries **eight**
built-in configs, two flagged recommended; there is **no
`getDefaultBuiltinConfigName()`** on the registry, so `ocio://default` is carried
literally; **`CreateFromFile` takes a builtin URI as happily as a path**, so
there is one entry point rather than a branch on the string's shape; and
`ocio://default` **is** `cg-config-v4.0.0_aces-v2.0_ocio-v2.5` (25 colour spaces,
8 displays, same defaults), so the dialog naming the concrete URI agrees with
`defaultConfigString()` in substance while being more durable than an alias.

### The input default is the scene_linear role, and both configs prove why

| config | `scene_linear` role | `getColorSpaceFromFilepath(".exr")` |
|---|---|---|
| Redshift `config.ocio` | **ACEScg** | `Raw` |
| `ocio://default` | **ACEScg** | `ACES2065-1` |

Stage 0 recorded the first row. **The second row is new, and IT IS THE DANGEROUS
ONE, precisely because it is plausible.** `ACES2065-1` really is a scene-linear
space, so a picture built on it looks entirely convincing and is simply wrong in
its PRIMARIES -- wrong saturation with correct-looking contrast, which a reviewer
would read as a grade rather than as a fault. `Raw` at least looks obviously
flat and gets noticed. **Every API call succeeds either way, and nothing but this
rule separates them** -- which is why the default is the ROLE, in the dialog, in
`setConfig()` and in the selftest alike.

**IF YOU ARE STARTING COLD, THIS IS THE ONE RULE TO CARRY OUT OF THIS DOCUMENT.**
Never default an input colour space from `getColorSpaceFromFilepath()`, on any
config, for any file type.

### The dialog

`src/app/ColorTransformDialog.{h,cpp}`. Config, input colour space, display,
view, each a combo populated from the resolved config; a status line naming the
config **and where it came from**; OK/Cancel.

- Changing the config repopulates the other three and does **not** carry names
  across -- a colour-space name from one config is how a dialog ends up naming
  something the config it compiles does not contain.
- Choosing Browse and then cancelling puts the combo back. Left on the Browse
  row, `selectedConfigString()` answers empty, which resolves to the DEFAULT
  config -- silently loading a different config from the one named is the exact
  thing this dialog must not do.
- A config that will not load **disables OK** rather than failing after it,
  where the previous configuration would silently stay in force and the dialog
  would look as though it had worked.
- Real Qt widgets, so it is screen-reader reachable by construction.

**THE TRANSFORM IS APPLIED ON OK, NOT LIVE. OWNER DECISION, 2026-08-24, SETTLED
-- NOT AN OPEN QUESTION AND NOT A DEFAULT TO BE IMPROVED ON.** Live preview would
recompile an OCIO processor on every combo change and, on video, issue a decoder
Step re-request from inside a modal dialog's event loop. **The owner declined to
spend that hazard on a comfort feature, on the grounds that the
see-it/don't-see-it comparison is already served by the `C` bypass rather than by
this dialog.** The reopen condition is stated and is his alone: revisit only if
it annoys him in real use.

### Which config is in force, on screen

The requirement was explicit. It is answered twice: in the dialog's status line,
and on the HUD, which now reads

```
map OCIO built-in ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5 / sRGB - Display / ACES 2.0 - SDR 100 nits (Rec.709)
```

Stage 2 left that field reading a bare `map OCIO`, which says the stage is
running and nothing about what it is doing -- and on an EXR the video line's
`xform` field is never built, so this is the only place that can say it.

### A defect the HUD caught on its first run

The first run under the new knob read **`map OCIO built-in ocio://default / /`**
-- config named, display and view **blank** -- on a transform that was
demonstrably working. `setConfig()` stored the configuration it was ASKED for,
and an empty display/view is filled in from the config's own defaults a few lines
later. It stores what it RESOLVED now, which is the rule `configLabel_` already
followed and the same rule the FFmpeg-root status line was fixed to.

### Persistence, and the missing-config fallback

One persistence function, so a LUT and a display/view configuration cannot be
written in disagreeing shapes; each clears the other's keys. **The config string
is stored RESOLVED** -- a path or an `ocio://` URI, never empty meaning "the
default" -- so a saved transform cannot change meaning because `$OCIO` was set or
unset between sessions.

A config that has gone falls back to **bypass**, says so **once** through the
transient message, and blocks nothing. Measured with its positive half, because
the failing half alone would also pass on a build that ignored the saved
transform entirely:

- config present: restores, engages, HUD names it
- config deleted, same settings: media **open**, `map Gamma 2.2 [...]`, and the
  toast reads *"Saved colour transform could not be restored - bypassed. Error
  could not read 'Z:/gone/missing.ocio' OCIO profile."* -- OpenColorIO's own
  message rather than one invented here.

---

## THE SELFTEST, BUILT BEFORE THE DIALOG

`--ocio-selftest` gains two assertions, both CI-safe (no file, no window).

**Exit 26** -- the resolved default config must carry **more than one colour
space** and at least one display. Not "a config loaded", which would pass on the
disabled raw config; the count is the signature that separates them.

**Exit 27** -- the `scene_linear` role and `getColorSpaceFromFilepath` must
**DIFFER**. It asserts a difference rather than a value, which is what makes it
durable: if a future OCIO made them agree, this fails and says the recorded
reason for preferring the role no longer holds, instead of leaving a comment
behind that no longer applies.

**Proven able to fail before being believed:**

| `$OCIO` | exit | reads |
|---|---|---|
| unset | 0 | `source=builtin spaces=25 displays=8 scene_linear=ACEScg`, differ=1 |
| the Redshift config | 0 | `source=env spaces=15 displays=1`, file rule `Raw`, differ=1 |
| a missing file | **26** | `spaces=0 displays=0` + OpenColorIO's own message |

---

## VIDEO IS UNMOVED -- measured against the same control as step 1

Control built from `2d09d89`, DLL payloads byte-identical by hash, binaries
proven distinct by their own strings.

| leg | HEAD | control |
|---|---|---|
| 4K H.264 x2 | **100.0 / 100.0%**, `drop 0`, `rephase 0`, `tick-late 0 of 119`, buckets `~1x 119` | **100.0 / 100.0%**, same |
| 4K H.264 `handler>budget` | **0 of 119** (max 4.4 / 4.7) | **0 of 119** (max 4.6 / 4.3) |
| 4444 x2 | **99.8 / 99.8%**, 261 frames, `drop 0`, `rephase 0` | **99.8 / 99.8%**, same |
| 4444 `handler>budget` | **0 of 260** (max 33.2 / 33.3) | **0 of 260** (max 34.1 / 34.4) |
| `scrubbar.ps1` full pool | **PASS -- 22 files, 88 legs, `delta 0` throughout** | -- |
| four selftests | green | -- |
| `verify_trace_assets --strict` | green, **33 embedded files** | -- |

`xform none` on every cadence run: the stage is off, which is the shipping
default. HEAD sits at or below the control's spread on handler max on both files.

---

## THE HARNESS, AND THREE TRAPS THAT COST A RUN EACH

`scripts/measure/colordialog.ps1` -- `open` / `apply` / `persist` / `missing` /
`guard`.

**`Focus-Window`, verified by reading `GetForegroundWindow()` back.** Without it
`Alt+V` went to the harness's own terminal and the run reported the dialog
missing on a build where it opens perfectly. A denial is reported as a HARNESS
failure, because "dialog not found" reads exactly like a build with no dialog.

**The guard leg's control sends `]` TWICE, and the first version read 0% on a
working build.** Pass 1/9 is the root layer and pass 2/9 is `Beauty` -- which
stage 2 measured as *the same render written twice*, differing only by
independent DWAA compression at 0.36% mean absolute difference. One press
therefore moves to a visually IDENTICAL pass. The script reported **INCONCLUSIVE**
rather than passing, which is the instrument being right about its own inputs;
two presses reach Cryptomatte, which displays as raw numeric IDs.

**SendKeys reserves both brackets** and swallows them unescaped -- it bit the
control leg even though the guarded loop already escaped them. And the
missing-config leg writes **forward slashes**, because QSettings' INI format
escapes a backslash and a hand-written Windows path would be read back mangled --
the run would then "prove" the fallback works when what it proved is that a
corrupt path does not load.

**The shortcut guard result**, with its negative control: `]` x2 with no dialog
moves **100%** of sampled picture pixels; `]`, `]` and `c` with the dialog open
move **0%**. The modal window is a separate window, so the main window's QAction
shortcuts do not fire -- verified, as phase 7's rule requires of any new surface.

`warnOnDuplicateMnemonics()` and `warnOnShortcutCollisions()` print **exactly the
four recorded pre-existing lines** and no new one.

---

## REVERTABILITY -- checked, and it is a STACK rather than four independent commits

| commit | alone | in order (newest first) |
|---|---|---|
| `0841201` resolved config + knob | **clean, builds** | clean |
| `84a9f3f` harness | **clean** | clean |
| `d16a98e` the dialog | **conflicts** | clean |
| `0e7d7c6` config discovery | **conflicts** | clean |

**The dependencies are real, not adjacency accidents.** The dialog cannot exist
without the enumerators `0e7d7c6` introduces, and `0841201` edits the very code
`d16a98e` added. Reverting the three in order is clean **and the reverted tree
builds** -- checked, not assumed -- which is the same shape stage 2 recorded for
the grouper pair (`bb159db` is a PREREQUISITE, not a sibling).

**THE ORDER, SPELLED OUT, so nobody has to re-derive it from the table:**

```
git revert --no-commit 0841201   # resolved config + TRACE_COLOR_VIEW
git revert --no-commit d16a98e   # the dialog
git revert --no-commit 0e7d7c6   # config discovery + the two selftest assertions
```

`84a9f3f` is the harness and is independent of all three -- revert it whenever,
or leave it. Taking `0841201` out ALONE is also clean and builds, and is the
right move if the only thing wanted back is the pre-`TRACE_COLOR_VIEW` state.

---

## WHAT IS OPEN, AND IT IS THE OWNER'S

**The dialog is done and correct. The performance answer is a stop, not a
to-do.** Per the instruction, nothing was optimised and no further work was
begun. The decision the measurement forces:

- An ACES display transform on a 1080p EXR sequence costs **~92 ms per frame** on
  the CPU, in parallel bands, against a 41.67 ms budget. It cannot be tuned into
  budget -- it is 2.2x the whole budget on its own.
- A **LUT** is free, and is in fact cheaper than no transform. So the LUT
  workflow stage 1 shipped is unaffected by any of this.
- **Stage 5, the GPU stage, is what would make an ACES view transform real-time.**
  OCIO builds a GPU shader from the same processor. Whether that is worth pulling
  forward is the owner's call and was not taken here.

**AND WHAT IS NOT OPEN, because a cold reader will otherwise go looking for it:**

- **STAGE 4, CRYPTOMATTE, IS CUT BY THE OWNER (2026-08-24). CUT, NOT DEFERRED.**
  He tested it and ruled that showing the cryptomatte PIXELS is all that was ever
  wanted. The grouper classifies `Crypto*` as `Data`, so it displays through
  `map Raw` -- raw numeric IDs with no view transform over them -- and it cycles
  with `[` and `]` like any other pass. There is no manifest reading, no
  rank-pair modelling and no ID picking, and **none of that is outstanding
  work.** Do not list it as unfinished business and do not pick it up as such.
- **Stage 2 is closed.** `[`, `]`, `C`, the pass overlay, the View pass list and
  `duplicateOf` are all built and measured.

**TWO THINGS ARE SETTLED RATHER THAN OUTSTANDING (owner, 2026-08-24), and both
are recorded here so a later session does not read them as gaps:**

- **Live preview: DECLINED.** See above. The dialog stays apply-on-OK.
- **A Look control: DECLINED.** Nothing in the asset set uses a Look, and
  `Config::look` is compiled and reachable -- **the same position
  `Kind::DisplayView` itself was in before this stage**, which the owner named as
  the right amount of readiness. Do not build it speculatively.
