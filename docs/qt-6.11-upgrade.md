# Qt 6.11.2: dev and CI together, measured

Step 3 of the toolchain modernisation. CI moves **6.7.2 → 6.11.2** and the
development box moves **6.10.2 → 6.11.2**, so the artifact and the build it is
measured on are the same Qt for the first time.

**6.10.2 was deliberately NOT taken as an intermediate milestone** (owner
decision, 2026-08-22). The assessment proposed it as an isolating experiment for
the window-drag audio dropout; that experiment had already been run and refuted
on `diag/audio-pull-gap-instrument` before the assessment was written. 6.7.2 and
6.10.2 gap identically, and this session adds 6.11.2 to that list.

Qt 6.10.2 is **kept installed** at `C:\Qt\6.10.2` as the control.

## The aqtinstall blocker, verified rather than assumed

`install-qt-action@v4` pins `aqtversion ==3.3.*`, and no released aqtinstall can
fetch 6.11.x — Qt changed its download-repo folder layout at 6.11.

Confirmed with a control rather than taken from the assessment: aqt 3.3.0
resolves 6.10.2's arch list fine, and fails **6.11.1 and 6.11.2 identically**
with `Failed to download checksum for the file 'Updates.xml'`. Same command, same
network, one version apart — a layout change, not a mirror blip.

**Pinned to a commit, not to master.** Floating CI on aqtinstall's branch would
reintroduce precisely the drift the vcpkg pin removes. The workflow uses
`git+https://github.com/miurahr/aqtinstall.git@16db45a70b5905ad596941b223469bc86a56901e`
— the commit the development box installed and validated 6.11.2 with, verified
to install and reach 6.11.2 from a clean venv before being written down.

## Two of the three named 6.11 risks do not apply

The assessment counted grep hits; these are the code sites.

| named risk | reality |
|---|---|
| `WM_DPICHANGED` "eight sites" | **one handler**, `MainWindow.cpp:5134`. The other seven hits are comments. |
| `WS_EX_LAYERED` "four sites" moved out of `CreateWindowEx` (QTBUG-135333) | **two calls**, and `TopChrome.cpp:227` **already** applies it with `SetWindowLongPtrW` after creation — which is what Qt changed *to*. Cannot bite. |
| Frameless `showMaximized()`/`showNormal()` rework (QTBUG-145092) | **Trace is not frameless.** Roadmap step 12 was closed as declined; the native title bar stays. Does not apply. |

**The one that was real is narrower than described.** Qt 6.9 removed legacy mouse
handling, but the risk is not Qt's internal routing — the D3D11 surface runs its
**own** Win32 window proc on its own child HWND and handles eight input messages
directly (`D3D11VideoRenderer.cpp:159–238`). The exposure is that if Qt enables
pointer input process-wide, that proc stops seeing `WM_MOUSEMOVE`/`WM_LBUTTONDOWN`.
Measured below: it does not.

## Measured, physical panel 5120x1440 @ 239.999Hz

Control is the same commit built against Qt 6.10.2 + FFmpeg 9.0.1.

| check | result |
|---|---|
| Build | clean, **no new warnings** (the three C4834 are pre-existing `QFile::open`) |
| Renderer selftest | `renderer=d3d11 fellback=0 planar=1` |
| Shape selftest | `OK - 11 shapes x 4 scale factors` |
| **`overlay.ps1` d3d11** | **all legs PASS**, loop accent **0/68/0** — the recorded figure |
| **`overlay.ps1` cpu** | all legs PASS, loop accent 0/41/0 |
| **`scrubbar.ps1` full pool** | **PASS — 22 files, 88 legs, `delta 0` throughout**, exit 0 |
| 4K H.264 cadence ×2 | **100.0/100.0%**, `drop 0`, `rephase 0`, `0 of 119`, all 119 gaps `~1x` |
| `transitions.ps1 -All` | **25 of 25 PASS** |
| `uiatree.ps1` | nine named controls + **MenuBar + five MenuItems** on real rects |
| `emptystate.ps1` | launch / transport / close / swap **all PASS**, plus the `-Bar` control at the recorded **641-row** stage |
| `topchromefade.ps1 -Mode rest` | d3d11 **RESTING TRANSLUCENCY (alpha 215), MAE 0.21**; cpu **OPAQUE, MAE 0.1** — both the recorded values exactly |
| Maximize / restore | identical to 6.10.2, **exact round trip** on both |

**The mouse question is answered.** Every overlay interaction leg passes on the
d3d11 default, which is the backend whose surface owns its own window proc. The
transport strip's hit regions still receive the messages they handle.

**The font question is answered, and there is zero drift.** 6.8 moved the Windows
font backend from GDI to DirectWrite, and the empty state's hint line is *text*,
so it is the sensitive measurement. It reads **169x14, gap 44, mark 59x68, offset
+0.5** — the recorded figures to the digit, on **both** backends.

One real behavioural change, benign: **Qt 6.11.2 enumerates 13 `Segoe UI
Variable` families where 6.10.2 enumerated 1** (the optical cuts — Display, Text,
Small and their weights — are now exposed individually). Both resolve to the
design's own `Segoe UI Variable` and both render identically, so nothing here
depends on it; it would matter to any code doing family matching by enumeration,
and nothing in Trace does.

## The caption-press stall is UNCHANGED, and QTBUG-132285 is not its attribution

The assessment hoped QTBUG-132285 / QTBUG-115992 ("window containing native
windows window is excessively repainted on move", fixed in 6.11.0) might close
the unattributed process stall for free. It does not.

Measured with the diag branch's own instrument built against Qt 6.11.2 in a
throwaway worktree, on the 90s tone file so every leg is inside the material —
per that document's own trap, a `silence` figure past the clip's end is
end-of-stream padding rather than the fault. `under 0` and `silence 0 B`
throughout.

| leg | Qt 6.7.2 (recorded) | Qt 6.10.2 (recorded) | **Qt 6.11.2 (this session)** |
|---|---|---|---|
| idle 10s (control) | 52.2ms `snap x0 dry 0` | 50.8ms | **50.5ms `snap x0 dry 0`** |
| move drag 10s | 142.5ms `snap x0 dry 1` | 153.4ms | **126.3ms `snap x0 dry 1`** |
| caption hold 10s | 555.6ms `snap x1 dry 1` | 527.9ms | **507.4ms `snap x1 dry 1`** |
| caption hold 30s | 547.7ms `snap x1 dry 1` | 530.9ms | **526.2ms `snap x1 dry 1`** |

Same fault, same signature, same class on all three Qt versions. `dry 1` at both
10s and 30s confirms it is still **once per press** rather than for the length of
the hold. **The version is still not the variable**, which is now measured across
three Qt majors rather than two.

**Why the hypothesis was always weak, stated so it is not re-proposed**:
QTBUG-132285 is repaint-on-**move**, and the worst reproduction of this fault is
a **motionless** caption press — 3.7x worse than dragging. A fix for repainting
while moving cannot explain a stall with no movement in it. The move-drag leg is
the one where it could have helped, and it reads 126.3ms against 142.5/153.4 —
inside the spread of the other two versions, not a fix.

**What stalls the process for ~500ms on a caption press remains unattributed.**

## Not testable here

**Mixed-monitor DPI did not run.** It needs two displays at different scale
factors; the second display was disconnected and multi-display work was withdrawn
by owner decision on 2026-08-15. `WM_DPICHANGED` handling moved in 6.11 (the
suggested rect is now passed into `checkForScreenChanged()`), and Trace has
exactly one handler for it, so this is the one named 6.11 change that is in
Trace's path and **unverified**. `scripts/measure/dpimove.ps1` is ready for it if
the hardware ever returns; it sets `PER_MONITOR_AWARE_V2` and refuses to measure
without it.
