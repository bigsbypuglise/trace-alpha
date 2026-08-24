# EXR stage 2: the multilayer pass model

Record of what was built, measured and found. 2026-08-24, physical panel
5120x1440 @ 239.999Hz. Commits `bb159db` (grouper selftest + two fixes),
`5301e74` (duplicateOf), `5b7158e` (pass list + one route), `c73aedd` (pinned
Normalise range), on branch `exr-stage0-dependencies`. **Not merged.**

---

## READ THIS FIRST: most of the priority list was already built

The brief asked to "begin the actual multilayer EXR pass/grouping
implementation" with a nine-item priority list. Checked against the code before
anything was written, **items 1-7 were already done by part 1 and part 2**, and
one instruction rests on an expired premise:

| # | asked for | state before this session |
|---|---|---|
| 1 | enumerate all channels reliably | **done** — `spec.channelnames` over all `nchannels` |
| 2 | group the three conventions | **done** — `componentIndex` takes `r/red`, `g/green`, `b/blue`, `a/alpha`, case-insensitively |
| 3 | preserve original channel names | **done** — `ExrPass::rawNames` |
| 4 | stable ordered pass list | **done** — first appearance in the file, root first |
| 5 | fix channel-3-as-alpha; resolve by name | **done in part 1** — alpha is `channel[3] >= 0`, filled by name |
| 6 | selected pass renders correctly | **done** — float path + per-class display mapping |
| 7 | wire `[` / `]` to the real pass model | **done in part 2** — they call `setPreferredPass(passes[i].layer)` |
| 8 | temporary overlay naming the pass | partly — used the generic toast |
| 9 | pass list in View | **not built** |

**The ~224 MB/frame allocation is already gone.** `loadExr` reads only the
active pass's channel span — the comment in the code says so and the measurement
below confirms it: **23.7 MB per pass against 213.6 MB for all 27 channels.**
The instruction "do not optimize it yet, measure and record it" is answered by
recording that it was fixed in part 1.

So this session did the two unbuilt items, and then went looking for what was
actually *wrong* rather than reimplementing what was right. It found four
defects, three of them by building an instrument that did not exist.

---

## 1. The grouper selftest, and why it had to be synthetic

`Trace.exe --exr-channels-selftest` drives `groupExrChannels()` over **14
channel layouts**. Pure logic over a `QStringList`: no file, no OpenImageIO, no
window, so it runs in CI beside the shape and OCIO selftests.

**IT EXISTS BECAUSE THE ASSET SET HAS ONLY TWO OF THE THREE RECORDED
CONVENTIONS.** Measured with OIIO over every EXR in the pool:

| file | channels |
|---|---|
| `Beauty_Only/icecream0000.exr` | `R G B` |
| `EXR_SEQ/R2_OP_Stacks_01_00000.exr` | `R G B` |
| `MultlayerAces/icecream_passes0000.exr` | `R G B` + eight layers, all `.red/.green/.blue` — **including its own `Cryptomatte`** |

The upper-case-with-alpha form CLAUDE.md records from stage 0
(`CryptoMaterial.R/.G/.B/.A`) **has no file here.** The convention that cannot be
tested against real media is exactly the one that needs a test, and a synthetic
channel list is the only way to write it.

**Five invariants run on every case**, including those with no expectation table,
because they are the things whose violation is silent:

1. every channel is accounted for — displayed, or recorded as unplaceable;
2. every stored index is in range;
3. **a component slot holds a channel whose own name ends in that component** —
   resolution by identity, never by position;
4. no two passes share a display name;
5. a Colour pass has all three of R, G and B.

**Proven able to fail, twice.** Removing the long spelling from
`componentIndex` fails 4 assertions; re-merging standalone passes fails 1 — the
defect below, reproduced on demand. Restored: `OK - 14 channel layouts`.

---

## 2. Two defects the invariants found

**A bare channel and a layer of the same name were merged.** A file carrying `Z`
and `Z.R/.G/.B` **dropped the three layer channels entirely** — invisible, with
no error, caught as *"channel 1 appears in no pass"*. Standalone passes are their
own thing now, and colliding identities are disambiguated (`Z` and `Z (layer)`),
because `layer` is what `setPreferredPass()` stores and `displayName` is what the
menu prints.

**An alpha-only layer rendered black.** `mask.A` left every colour slot unset, so
the loader drew an empty frame with an alpha nothing reads: a pass that is
present, selectable and invisible. Alpha is replicated across RGB — the same
answer a bare data channel already gets.

Neither is reachable by any file in the pool. Both are the kind of thing a
malformed or unusual render produces, and both were silent.

---

## 3. Grouping rules, as they now stand

- **Split at the LAST `.`** — so `diffuse.light1.R` is layer `diffuse.light1`,
  component `R`. A channel with no dot is a root-layer channel.
- **Component suffix, case-insensitive, from a closed set**:
  `r|red` → R, `g|green` → G, `b|blue` → B, `a|alpha` → A. This is the one rule
  that spans all three conventions.
- **Anything else becomes its own single-channel pass**, keyed on its full raw
  name and replicated across R,G,B so it draws as grey rather than as a red-only
  picture. No channel in the file is ever dropped.
- **Class is decided per pass by EXACT name matches against small closed sets**,
  never `contains()`: "Specular" contains a *p* and "Reflections" contains an
  *n*, and a substring test sends both through a data mapping. Unknown name with
  ≥3 channels → Colour; otherwise → Data.
- **`Crypto*` is Data**, so its numeric IDs are shown raw rather than through a
  view transform. Cryptomatte *interpretation* is stage 4 and is not started.
- **Raw names are kept** in `ExrPass::rawNames` and printed on the HUD, so a
  convention we have not met shows up rather than silently finding nothing.

### Pass ordering

**First appearance in the file's channel list.** The root layer's channels come
first in every file measured, so the root is pass 1; each named layer follows in
the order its first channel appears. The order is therefore a property of the
file and is stable across frames of a sequence, which is what lets `[` and `]`
mean the same thing on every frame.

Measured on the 27-channel file — 9 passes:

```
1 (root)            colour    R,G,B
2 Beauty            colour    Beauty.red,Beauty.green,Beauty.blue      = (root) (0.36%)
3 Cryptomatte       data      Cryptomatte.red,...                      map Raw
4 DiffuseFilter     colour
5 GI                colour
6 P                 position  P.red,P.green,P.blue                     map Normalise
7 Reflections       colour
8 Shadows           colour
9 SpecularLighting  colour
```

### How root RGBA is chosen

`choosePass()` in order: the caller's **preferred layer name** if it matches one
(case-insensitive); else the **root group if it is Colour**; else the **first
Colour pass**; else pass 0. So a file with a root layer opens on it, and a file
with none opens on its first colour AOV rather than on whatever happened to be
first.

**The empty layer name round-trips, and it looks like it should not.** Cycling to
the root sets the preference to `""`, which `choosePass()` reads as "the file
decides" rather than as a match — but its first fallback is the root colour
group, which is the pass being asked for. The ambiguous case can only arise on a
file with no root pass, and on such a file no pass has an empty layer name, so
`""` is never what gets set.

### How alpha is resolved

**By name, never by position** — this was stage 0's recorded defect and part 1
fixed it; the selftest now asserts it. A pass has alpha iff some channel's own
suffix is `a` or `alpha`, and `hasAlpha()` is `channel[3] >= 0`. Nothing anywhere
assumes channel index 3.

The selftest case `components out of order in the file` is the proof: given
`foo.B, foo.A, foo.R, foo.G` the pass reads `ch 2,3,0,1` — R from index 2, alpha
from index 1. A positional grouper would have put blue in red.

When a pass has no alpha the loader **pre-fills 1.0** and the contiguous read
never touches it. The root group of every file in the pool has no alpha
(`alpha_ch -1` from OIIO on all three).

### How duplicate and ambiguous groups are handled

Two different questions, answered separately.

**Two channels claiming one slot** (a malformed file): the **first wins**, and
the loser is **recorded** in `ExrPass::ambiguous` rather than discarded. Taking
the later one would silently change which pixels are shown; dropping it without
trace is the failure nobody notices. The count is shown on the menu row.

**Two passes that are the same picture**: `duplicateOf`, declared and empty since
part 1, is now **measured from pixels**. The 27-channel file writes its beauty
render twice — once as root `R,G,B` and once as a `Beauty` layer.

**Bit-equality finds nothing**, which is the whole difficulty: the copies are
compressed *independently* with lossy DWAA, so only 5-7% of pixels are
bit-identical. The test is a **relative mean absolute difference over a band of
24 scanlines**, tolerance 2% of the signal, and the measured figure is carried to
the HUD and the menu so the claim can be checked:

```
pass 2/9 Beauty colour [Beauty.red,Beauty.green,Beauty.blue] = (root) (0.36%)
```

**0.36% is part 1's own independently measured MAD** (0.0035/0.0028/0.0048 on
values whose mean is ~1.0) arrived at from a different direction.

**No false positives on the other seven**, which is the half that matters:
Reflections, Shadows and SpecularLighting all peak below 1.5 and would have
matched each other under a naive absolute test. Two passes that are **both**
empty are deliberately not called duplicates — a relative measure is meaningless
when both sides are ~0, and two AOVs that happen to be black in the sampled
frame are a property of that frame, not of the render.

**One read covers every pass** — a band across all channels is 5.0 MB at 1080p
against a read per pass — and it is **cached on the file's joined channel
names**, which every frame of a sequence shares, so a 97-frame sequence pays it
once.

---

## 4. The pass list, the overlay, and one route

`View > EXR Pass` is a submenu of the real pass list carrying name, class and
duplicate. A submenu rather than flat rows, because a multilayer render can carry
dozens of AOVs and they would push every item below them off the bottom of View.
**The class is on the row** because it decides the display mapping, so a pass that
will be normalised says so *before* it is chosen.

**`applyExrPass()` is the one place a pass change happens** — `[`, `]` and every
menu row route through it — so the reload, the cache clear, the overlay and the
menu tick cannot disagree. The tick and the overlay both name the **loaded** pass,
never the requested one, because `choosePass()` falls back when a frame lacks the
requested pass.

**Rebuilt only when the pass list changes.** `syncExrPassActions()` runs from
`refreshHud()`, so an unconditional rebuild would destroy and recreate the menu
several times a second during playback — possibly while the user had it open. The
key is the joined display names, a property of the channel layout, so a 97-frame
sequence builds it once.

**Rows carry no mnemonics**: a pass name is file data, and letting it claim an
Alt key would make the menu's keyboard behaviour a property of whatever a
renderer called an AOV. `&` in a name is escaped.

**The overlay is the existing composited toast, deliberately** — it already draws
over the picture, outside the transport's fade, and expires on its own timer,
which is the whole of what "a small temporary overlay naming the pass" asks for.
A second mechanism would be the duplication roadmap step 3 removed from the empty
state. Measured on screen: **`Pass 7/9: Reflections  (colour)`**.

`warnOnDuplicateMnemonics()` caught this change's own collision on its first run
— `&Previous Pass` against `EXR &Pass`, both claiming `p` in one menu. It is
`E&XR Pass` now.

---

## 5. The defect the pass model exposed: Normalise was auto-ranging

The owner's stage-5 ruling requires every mapping to be *"a pure per-pixel
function of the float sample plus a few scalar parameters. No whole-frame
statistics. No auto-ranging min/max scan"* — because an auto-ranged pass
*"changes its mapping frame to frame, which makes the viewer untrustworthy"*.

`Normalise` was doing exactly that. **Measured on the P pass:**

| frame | before | after |
|---|---|---|
| 0 | `[-44.2500..44.2500]` | `[-44.2500..44.2500]` |
| 1 | `[-44.2500..44.2500]` | `[-44.2500..44.2500]` |
| 96 | **`[-39.2500..44.0000]`** | **`[-44.2500..44.2500]`** |

The same world position was a different grey depending on the playhead. The range
is measured from a pass's **first frame and pinned** for the rest of it. The `>1`
fraction still tracks each frame, because that is *reporting* — it says how much
this frame is clipping.

It is also the **stage-5 prerequisite**: a mapping needing a whole-frame
reduction cannot become a shader without a separate pass; two pinned scalars are
two uniforms.

**The pin is per pass and is proven so.** `setDisplayMap()` drops it only when the
*mapping* changes — it is called on every loaded frame — and `applyExrPass()`
clears it explicitly, because two position passes share a mapping and would
otherwise inherit each other's range. Measured: leaving the pass and returning
re-derives the range from the frame then on screen.

**One property worth knowing rather than fixing**: re-entering a pass at a
different frame pins a different range, because the pin comes from whichever
frame is showing when the pass is selected.

---

## 6. Keyboard behaviour against the real pass model

`scripts/measure/passkeys.ps1`, all seven legs **PASS** on the 9-pass file:

- `]` advances on **10 of 10** presses;
- the list returns to the opening pass after **exactly 9** presses — the wrap is
  the real pass count, not a coincidence;
- `[` undoes `]` with the media line byte-identical;
- menu-bar focus: `popups 0->0` on `[`, `]` and `c`, with the brackets still
  running;
- text-field guard holds — the `QLineEdit` reads `c[]hjkltefsm`, picture moved
  **0%**, against **99.9%** for the same key outside;
- `[`/`]` move the picture **0%** on video (the actions are disabled);
- Ctrl+C and Ctrl+L unchanged.

**Pass selection survives frame changes**, which the sequence path requires:
selected `P` at frame 0, then stepped to 1, 4 and 96 — `pass 6/9 P position` on
every one.

---

## 7. Memory and performance

**Read cost per channel span** (`exrprobe --read`, best of 5, 27-channel DWAA
1080p file):

| span | best | mean | floats |
|---|---|---|---|
| all 27 channels | 62.85 ms | 66.17 ms | **213.6 MB** |
| root RGB (pass 1) | 35.35 ms | 36.08 ms | **23.7 MB** |
| Beauty (pass 2) | 35.86 ms | 36.57 ms | 23.7 MB |
| P (pass 6) | 34.96 ms | 35.95 ms | 23.7 MB |
| SpecularLighting (pass 9) | 45.41 ms | 46.41 ms | 23.7 MB |

**One pass is a ninth of the memory and about half the time** — not a ninth of
the time, because DWAA decodes in blocks and there is fixed overhead. The last
span is ~10 ms slower than the first three, which is block traversal rather than
pixel count.

**Process working set**, 27-channel file, single frame open:

| | this session | part 1 recorded |
|---|---|---|
| working set | **246.6 MB** | 254.9 MB |
| peak | **351.7 MB** | 339.6 MB |

Flat against the record — the duplicate band read (5.0 MB, once per layout) and
the pass menu cost nothing measurable. **Sequence peak**: multilayer 97-frame
**354.0 MB**, R2_OP_Stacks 217-frame **278.0 MB**; both play to their last frame
(`Frame: 96/96` and `Frame: 216/216`).

**Float frame arithmetic is unchanged from part 1**: `w x h x 16` bytes, window
cache of radius 1, so 1080p is 33.2 MB per frame / 99.5 MB cached, 4K 132.7 MB /
398 MB, 8K 530.8 MB / **1.59 GB**. The 4K and 8K rows are arithmetic — there is
no 4K or 8K EXR in the asset set.

**A shape worth recording before it bites**: the loader's fast path needs the
pass's channels to be **contiguous and in R,G,B(,A) order**, which is what OIIO
presents for every pass in the pool. A file whose layer channels are *scattered*
falls to the general path, which reads the whole **span** between the first and
last channel — for a pass with one channel near the start and one near the end,
that is the whole file again. Not reachable by any file here; it is the case to
test if the 224 MB ever comes back.

---

## 8. Regression at HEAD, flat

- `scrubbar.ps1` full pool **PASS — 22 files, 88 legs, `delta 0` throughout**,
  4.9 min.
- 4K H.264 cadence x2 **100.0 / 100.0%**, `drop 0`, `rephase 0`,
  `tick-late 0 of 119`, `handler>budget 0 of 119`.
- 4444 cadence x2 **99.8 / 99.8%**, `drop 0`, `rephase 0`, `0 of 260`.
- Four selftests green: `renderer=d3d11 fellback=0 planar=1` · `trace-ocio
  version=2.5.2 … moved=1` · `OK - 11 shapes x 4 scale factors` ·
  **`trace-exr-channels: OK - 14 channel layouts`**.
- `verify_trace_assets --strict --no-pillow` exit 0 at **33 embedded files**.
- `passkeys.ps1` all seven legs PASS.

**Revertability, checked rather than asserted.** `c73aedd`, `5b7158e` and
`5301e74` each revert cleanly *and* the reverted tree builds. **`bb159db` does
not revert alone** — the pass menu reads `ExrPass::ambiguous`, which that commit
introduces, so it is a genuine prerequisite rather than an adjacency accident.
Reverting the pair in order (`5b7158e` then `bb159db`) is clean and builds.

---

## What remains before Cryptomatte and EXR playback optimisation

**Before Cryptomatte (stage 4):**

1. **Cryptomatte is currently `Data` + `map Raw` by design** — its channels are
   numeric IDs shown raw, never through a view transform. Interpretation needs
   the manifest from the header (`cryptomatte/<hash>/manifest`), which nothing
   reads yet.
2. **The rank-pair convention is not modelled.** Real Cryptomatte writes
   `CryptoMaterial00.R/.G/.B/.A` as *(id, coverage)* pairs, not colour. The
   grouper currently groups those as an ordinary RGBA layer, which is the right
   *neutral* answer but not the right *interpreted* one.
3. **The pool has no real Cryptomatte to test against** — this file's
   `Cryptomatte.red/.green/.blue` is a 3-channel preview, not a ranked set. Test
   material is needed before any interpretation is written.

**Before EXR playback optimisation:**

4. **The float frame is the cost, not the read.** 33.2 MB per 1080p frame and a
   3-frame window; 8K would want 1.6 GB. **Bound the window cache by BYTES rather
   than frame count** before any 4K/8K EXR arrives.
5. **The sequence path exposes no cadence counters** — no `drop`, no `rephase`,
   no presented-rate figure — so *no EXR playback rate has ever been measured* in
   this project. That instrument has to exist before anything is optimised, or
   the result cannot be judged.
6. **Half the per-pass read cost is fixed overhead** (35 ms for 3 channels
   against 63 ms for 27). A prefetch that overlaps it is the obvious lever and is
   unmeasured.
7. **`measureFloatRange()` still scans every frame** for the HUD's clipping
   figure, even where the mapping no longer needs it. Cheap at 1080p, and the
   first thing to make optional if EXR playback is ever tight.
