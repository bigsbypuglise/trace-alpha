# Playback speed through pause/play — the audit (2026-08-20)

Tester-driven item: playback speed does not survive pause/play. The instruction was to audit
WHICH case this is before changing anything — the Playback Speed menu, or the shuttle ladder —
and to get the owner's call before touching the shuttle. Both cases are audited here; one
one-line defect was found and fixed (`f8dbbd6`); **the behaviour itself is unchanged in both
cases, and making either sticky is an owner call, framed at the bottom.**

## Case 1 — the Playback Speed menu: NOT a setting, and no comment says it is

The item's premise was *"the Playback Speed menu is a setting, and the code comment says so"*.
The audit found the opposite, in four places:

- The spec (interface-pass-1-spec.md:404): *"The checked item must reflect the effective
  playback rate."*
- `MainWindow.h:237`: *"Tick the Playback Speed menu from playback_.state().speed. Read back
  from the controller, never remembered, so the menu cannot claim a rate the engine is not
  running."*
- `MainWindow.h:844`: *"Speed is a menu over the ONE rate machine … the checked item is read
  back from playback_.state().speed rather than remembered here. Two sources for 'what rate is
  in force' is exactly what the spec's shared-actions rule exists to prevent."*
- `syncPlaybackSpeedActions()`'s own comment: *"A menu that remembered what it last set would
  claim 10x over a stopped file."*

So the shipped design is: a menu item is a **command** ("play at this rate now"), and the
checkmark mirrors the engine. `PlaybackController::pause()` zeroing `state_.speed` is that
design working — a paused engine runs no rate, so no item should be ticked — and
`togglePlayPause()` setting 1.0 on Play is likewise deliberate. **Phase 14 part 1's owner
acceptance covers this**: "Loop, 0.5× and Copy Current Frame are ACCEPTED. All three behave as
intended."

**The defect that WAS real: the sync was not called where the rate most often changes.**
`syncPlaybackSpeedActions()` had eight call sites — `setPlaybackSpeed`, menu construction, and
the tick's five run-off-the-end paths — and *none* on the Space/K pause path or on a shuttle
press. So pausing a 0.5× run left "0.5x" ticked over a paused file, and pressing Play then ran
at 1× with the menu still claiming 0.5× — the checkmark disagreement the tester saw, produced
not by pause "discarding a setting" but by the mirror not being updated. Fixed with one call in
`refreshHud()` (the same one-place-after-every-transport-action reasoning the
`playbackAtEnd_` clearing already uses there). Verified on screen: 0.5× ticked while running
0.5×, nothing ticked paused, Normal ticked after Play.

## Case 2 — the shuttle ladder: spec'd and signed off, in the owner's process

The spec's Rewind section (interface-pass-1-spec.md:368–369):

> Pressing Play returns to normal +1× playback.
> Pressing Pause stops and clears the active shuttle rate.

And the phase 16 verification checklist (:611): *"Play restores 1×; Pause clears shuttle
state"* — re-verified at phase 5's 25-case transition matrix, all PASS. Making the shuttle
rate survive pause/play is therefore a **spec reversal**, not a bug fix, and was not touched.

## The owner call, framed

If testers want the *menu* rate to survive pause/play (set 0.5×, pause, play → resume at
0.5×), that is a change from "menu items are commands" to "the menu rate is a mode", and it
needs decisions the code cannot take alone:

1. **What does Space mean after a menu rate?** Today Play means 1× everywhere. A sticky menu
   rate makes Space resume at 0.5× (or 10×) — while a shuttle rate must NOT stick, per the
   spec above, so J/L rates and menu rates would part ways in what pause does to them. That
   distinction is expressible (the controller knows which command set the rate) but it is a
   new rule the spec does not have.
2. **Audio.** Audio drives at exactly 1× and nothing else — `audioShouldDrive()`'s `== 1.0`
   is settled phase 14 behaviour. A persistent 0.5× is persistently silent; a user who paused
   at 0.5×, came back the next day (if it also persisted), and pressed Play would report
   "sound is broken". If any stickiness is taken, the silence at off-speeds needs to be
   visible (the rate chip helps; the menu tick now does too).
3. **Scrub-release restore.** `userPlayIntent_` deliberately restores only 1× after a drag
   ("0.5x and the fast rungs are deliberate gestures, and resuming one at 1x would be the
   wrong answer" — setPlaybackSpeed's own comment). A sticky rate reopens what a release
   should resume at.

Recommendation, if the call is wanted: keep the shuttle exactly as spec'd; make only the
*menu* rate sticky across pause/resume within a session (not across files, not across
sessions), with Space resuming the ticked rate and the rate chip flashing it. But that is a
recommendation for the owner to accept or decline, not a change this session made.
