#include "app/MainWindow.h"

#include <QScreen>
#include <QWindow>

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QDragEnterEvent>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QLocale>
#include <QMenuBar>
#include <QMessageBox>
#include <QPointer>
#include <QPushButton>
#include <QSettings>
#include <QThreadPool>
#include <QMimeData>
#include <QResizeEvent>
#include <QStatusBar>

#include <QClipboard>
#include <QCoreApplication>
#include <QGuiApplication>
#include <QDesktopServices>
#include <QEventLoop>
#include <QSlider>
#include <QUrl>
#include <QUrlQuery>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#ifdef Q_OS_WIN
// For the WM_SIZING / WM_ENTERSIZEMOVE / WM_EXITSIZEMOVE counters in
// nativeEvent(). NOMINMAX because <algorithm> above is what this file uses for
// std::max, and windows.h defines a max() macro that breaks it.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include "ui/OverlaySpike.h"
#include "ui/ViewerWidget.h"
#include "render/OverlayModel.h"
#include "ui/TransportOverlay.h"
#include "ui/TransportBar.h"
#include "app/LucidLinkIntegration.h"
#include "app/OverlayAccessibility.h"
#include "app/Settings.h"
#include "app/ShortcutsWindow.h"
#include "app/WindowShape.h"
#include "core/MediaIoSource.h"
#include "core/SequenceParser.h"
#include "core/TimeFormat.h"
#include "core/VideoFrameSource.h"
#include "core/ImageSequenceFrameSource.h"

namespace trace::app {

using trace::core::MediaKind;
using trace::core::PlaybackMode;
using trace::core::PrimaryReadoutMode;

namespace {

// Coalescing window for slider events, so a burst of moves costs one decode
// rather than one each. Deliberately NOT used to pace shuttle catch-up.
// Spec section 4: "reserve a safe margin around the outer window". Per side, in
// logical pixels, applied to both axes -- so a source that exactly matches the
// work area still opens with the frame visible rather than flush against a
// taskbar edge. Deliberately modest: this reduces how large media can open, and
// a generous margin would make every 4K file open noticeably smaller than it
// needs to.
constexpr int kWorkAreaMarginPx = 24;

constexpr int kScrubCoalesceMs = 12;

// A gap between shuttle paints long enough to read as the picture stopping.
//
// ABSOLUTE, and that is the whole point. The `stalls` counter beside it is
// `gap > 2 x refresh`, which on this box is 8.33ms at 239.999Hz and 33.3ms at
// 60Hz -- so the same drag on the same build scores four times worse on the
// faster panel, and every stall figure recorded in the plan is silently in
// whichever unit that session's display mode happened to impose. The display
// mode on this box was observed changing between sessions, and no recorded
// stall number is tagged with a refresh rate.
//
// 33.3ms is where the project's own recorded hitches start ("30-116ms gaps
// where a cache miss forces a seek and a GOP walk") and it is close to a frame
// at 24fps, so a hitch here means the picture held for about as long as a
// played frame. What matters more than the exact value is that it does not
// move when the monitor does.
constexpr double kScrubHitchMs = 33.3;

// Fraction of the remaining distance a shuttle slice covers. Sets how tightly
// the picture tracks the pointer: the steady-state lag under a constant drag
// is roughly (frames the pointer moves per slice) / kScrubEase, so halving the
// lag means doubling this. 0.25 was measured as too loose -- "lagging too far
// behind" on a quick drag -- and 0.5 tracks closely while still decelerating
// into the target rather than arriving with a jolt.
constexpr double kScrubEase = 0.5;

// Fraction of a display refresh interval that must pass between shuttle
// presentations. 1.0 means at most one frame per refresh -- every painted
// frame is genuinely shown, and a run of cheap cache hits can no longer arrive
// as a burst that the panel samples once. Lower values allow bursting again in
// exchange for closing the pointer gap faster.
//
// Minimum interval between scrub repaints, as a fraction of a display refresh.
//
// DEFAULT IS 0 -- paint every decoded frame. Pacing was re-tried in Aug 2026
// with a better mechanism (it no longer breaks out of the walk, so it does not
// throttle the decoder the way the first attempt did) against a new metric,
// and the measurement did not support turning it on:
//
//   4K H.264 fast scrub   wasted 98% -> 43%,  stalls 7 -> 8,   max gap 102 -> 116ms
//   1080p    fast scrub   wasted 97% -> 26%,  stalls 21 -> 34, max gap 78 -> 85ms
//
// It does what it claims -- far fewer frames painted into a refresh interval
// that could never show them -- and that turns out not to be worth anything.
// A paint costs 0.23-0.36ms, so ~600 wasted paints is ~200ms across an entire
// multi-gesture run, and suppressing them left the stall count unchanged at 4K
// and clearly worse at 1080p.
//
// The lesson is about which quantity matters: burstiness is not what a drag
// feels like, STALLS are -- the 30-116ms gaps where a cache miss forces a seek
// and a GOP walk. Nothing about paint scheduling can reach those. Don't come
// back to pacing; make misses cheaper or rarer.
//
// TRACE_SCRUB_PACE: 0 paint every decoded frame (default), 1.0 one frame per
// display refresh, values between.
double scrubPaceFraction() {
    static const double frac = [] {
        const QByteArray raw = qgetenv("TRACE_SCRUB_PACE");
        if (!raw.isEmpty()) {
            bool ok = false;
            const double v = raw.toDouble(&ok);
            if (ok && v >= 0.0 && v <= 4.0) return v;
        }
        return 0.0;
    }();
    return frac;
}

// The two knobs that decide how much of the UI thread a synchronous shuttle
// slice takes: how long one slice may spend decoding, and how soon the next
// slice is re-armed once it is still behind the pointer. Both are settable so
// the async win can be measured against a scheduler tweak rather than confused
// with one -- shipped defaults are the values that were already hard-coded.
double scrubWalkBudgetMs() {
    static const double ms = [] {
        const QByteArray raw = qgetenv("TRACE_SCRUB_WALK_MS");
        if (!raw.isEmpty()) {
            bool ok = false;
            const double v = raw.toDouble(&ok);
            if (ok && v >= 0.0 && v <= 200.0) return v;
        }
        return 8.0;
    }();
    return ms;
}

// The most frames one asynchronous shuttle request may cover.
//
// WHY THERE IS A BATCH AT ALL. The async chain posts one frame per cross-thread
// round trip. On a 1280x720 H.264 source a frame decodes in 0.12ms and was
// being delivered every 3.56ms -- 97% of the interval is the round trip -- so
// the picture ran 76 frames and 236ms behind the pointer on a fast drag while
// the decoder sat idle. The synchronous walk never showed this because one
// slice covered `ceil(gap * kScrubEase)` frames inside a single call. That ease
// was dropped from the async path on the reasoning that the pipeline would
// supply the same acceleration by itself, which holds only while the round trip
// is small against a frame: true at 4K (3.87ms a frame) and at 1080p (0.68),
// false at 0.12.
//
// WHY 4. Effective cost per frame is roundTrip/N + decode, so on that file
// N=1 is 3.56ms (281 f/s), N=2 is 1.84ms (543 f/s) and N=4 is 0.98ms
// (1020 f/s). The fastest pointer demand measured anywhere in the asset set is
// 479 f/s, so 4 clears the worst measured gesture by roughly 2x while 2 barely
// clears it; above 4 the headroom is against nothing that has been observed.
// Two costs grow with it and both bind before any benefit does. The conversion
// pool's floor is `clamp(maxEntries, 4, 128) + 4`, i.e. 8 slots on the largest
// media, and a batch holds its frames alive simultaneously -- 4 is half that
// floor, so it cannot reintroduce a per-frame allocation during a drag, which
// is when it is least affordable. And a batch delays its own first frame, which
// is bounded separately by the budget below.
//
// 0 or 1 restores one frame per request, i.e. the behaviour this replaces. It
// is the negative control in the same binary rather than a build.
long long scrubBatchCap() {
    static const long long n = [] {
        const QByteArray raw = qgetenv("TRACE_SCRUB_BATCH");
        if (!raw.isEmpty()) {
            bool ok = false;
            const int v = raw.toInt(&ok);
            if (ok && v >= 0 && v <= 64) return static_cast<long long>(v);
        }
        return 4LL;
    }();
    return n;
}

// Whether random-access scrub decode runs on the worker. Default on; the
// switch exists because the synchronous walk is the validated path and a
// regression should be one env var away from being isolated, not a revert.
bool asyncScrubEnabled() {
    static const bool on = [] {
        const QByteArray raw = qgetenv("TRACE_ASYNC_SCRUB");
        if (raw == "0") return false;
        return true;
    }();
    return on;
}

// Whether the EXACT landing -- a groove click, a slider release, a frame step --
// is decoded on the worker instead of on the UI thread. Default on;
// TRACE_ASYNC_LANDING=0 restores the synchronous landing in the same binary and
// is the negative control for every figure this change is judged on.
//
// It changes WHERE the decode runs and nothing else. The request is still
// RequestMode::Step, still one frame, still full resolution with the accurate
// conversion, and the frame that arrives is still the frame that was asked for.
// The owner's ruling of 2026-08-14 is what scopes it that narrowly: VLC
// struggles with the single-GOP Seedance file too, so no player is showing an
// approximate frame during that gesture and exactness is not what costs Trace
// anything there. What costs it is that showing frame 57 of a 97-frame,
// one-keyframe file requires decoding 58 frames -- 8.9ms each -- and Trace was
// doing all 58 inside the mouse handler. The window was dead for 261-585ms.
//
// So this makes 520ms not a freeze. It does not make it shorter, and nothing
// here claims to.
bool asyncLandingEnabled() {
    static const bool on = [] { return qgetenv("TRACE_ASYNC_LANDING") != "0"; }();
    return on;
}

// Checkpoint 2 stage one. How many frames ahead of the presentation point
// ordinary 1x forward playback may decode. DEFAULT 0 = OFF, so this commit
// changes nothing until it is measured and reported.
//
// Depth is shallow on purpose and is bounded by BYTES as well as by count -- a
// full-resolution 8K 12-bit 4:4:4 frame is ~199MB, so a depth of 3 would be
// ~600MB on top of the 384MB reverse cache and a working set already near
// 900MB at 4K. The two budgets do not know about each other and this
// deliberately does not make them: the reverse cache is bounded and verified
// independently (plan section 26.5) and coupling them would put a second policy
// inside a settled one. They are summed and reported instead.
//
// THE NUMBER IS TO BE JUSTIFIED BY THE MEASURED STARVATION COUNT, NOT CHOSEN.
int playbackQueueRequestedDepth() {
    static const int n = [] {
        const QByteArray raw = qgetenv("TRACE_PLAYBACK_QUEUE");
        if (!raw.isEmpty()) {
            bool ok = false;
            const int v = raw.toInt(&ok);
            if (ok && v >= 0 && v <= 16) return v;
        }
        return 0;
    }();
    return n;
}

// Byte budget for the prefetch, separate from TRACE_REVERSE_CACHE_MB.
int playbackQueueBudgetMb() {
    static const int n = [] {
        const QByteArray raw = qgetenv("TRACE_PLAYBACK_QUEUE_MB");
        if (!raw.isEmpty()) {
            bool ok = false;
            const int v = raw.toInt(&ok);
            if (ok && v >= 16 && v <= 4096) return v;
        }
        return 512;
    }();
    return n;
}

// GATE E step 1. Default on; TRACE_DEADLINE_SCHED=0 restores the pre-GATE-E
// fixed-interval tick and its wall-clock accumulator gate.
//
// This is a CONTROL SWITCH, selecting one of two whole schedulers -- it is not
// a runtime blend of them, and nothing reads it per frame to decide how much of
// each to apply. Plan section 24.3 forbids layering a second opinion about when
// to present; having both implementations behind one flag is how that rule gets
// tested rather than asserted.
//
// It exists because section 24.9 requires a negative control: a build that
// still shows the old 62-frame beat, proving the harness can see the thing the
// fix claims to have removed. Doing it with an env var rather than a control
// BUILD means the two runs differ in one branch instead of in a compile.
bool deadlineScheduleEnabled() {
    static const bool on = [] {
        const QByteArray raw = qgetenv("TRACE_DEADLINE_SCHED");
        if (raw == "0") return false;
        return true;
    }();
    return on;
}

// When a handler overran the frame period, re-arm at once instead of waiting for
// the next grid slot. Default ON; TRACE_SCHED_FREERUN_LATE=0 restores the pure
// grid rephase and is the control for every figure below.
//
// armNextPresent()'s rephase branch arms for the next slot STRICTLY AFTER now,
// which is right for a transient overrun: one long frame costs one slot and the
// run resumes at rate rather than fast-forwarding through arrears. It is wrong
// when the overrun is SUSTAINED, because then every arm inserts the remainder of
// a slot as idle and the achieved rate quantises to fps/N for integer N. On the
// 7680x4320 ProRes 4444 XQ plate the handler is 88ms against a 41.71ms period --
// 2.11 slots -- so it arms for slot 3 and plays at 23.976/3 = 7.99fps when the
// pipeline can supply 11.0. Measured: `outside` 32.22ms/frame of a 121ms period,
// `rephase` firing on all 44 frames, 34.4% of real time against 44.9% with the
// whole deadline scheduler switched off.
//
// The test is per frame and needs no new state: `lastHandlerMs_` is written by
// the recordHandler guard, which is declared AFTER the armNext guard and so runs
// BEFORE it. A handler that fit its period and is nonetheless late is jitter and
// keeps the grid; a handler that did not fit is a saturated pipeline and there is
// nothing to wait for.
//
// It is inert on every file that meets its budget, and that is measured rather
// than argued: `rephase` reads 0 across the whole validated asset set, so the
// branch this sits in never executes there. 4K H.264 x3 100.0/100.0/100.0% with
// `0 of 119`, 4444 x2 99.8% with `0 of 260`, 4K 60fps x2 100.0% with `0 of 161`
// against a 16.67ms budget -- case for case with the control. On the 8K plate it
// is 33.8% -> 45.6% of real time, and the cadence gets EVENER as well as faster
// (p50/p99 123.8/140.4 -> 89.7/100.9), so there is no steady-cadence argument
// for the quantised behaviour it replaces.
bool freerunWhenSaturated() {
    static const bool on = [] { return qgetenv("TRACE_SCHED_FREERUN_LATE") != "0"; }();
    return on;
}

// Preview sampling during an active drag. Default on.
//
// This is the one place Trace's "never skip a frame" rule is deliberately not
// in force, and the boundary is exact: it applies to the ACTIVE DRAG PREVIEW
// only. The release landing, stepping, playback and every exact request are
// untouched -- responsiveness beats completeness while the hand is moving,
// exactness beats speed the moment it stops.
bool scrubSamplingEnabled() {
    static const bool on = [] { return qgetenv("TRACE_SCRUB_SAMPLE") != "0"; }();
    return on;
}

// Ceiling on how far one preview step may advance. Bounded so a momentary bad
// estimate cannot turn a drag into a slideshow of distant frames.
int scrubMaxStride() {
    static const int n = [] {
        const QByteArray raw = qgetenv("TRACE_SCRUB_MAX_STRIDE");
        if (!raw.isEmpty()) {
            bool ok = false;
            const int v = raw.toInt(&ok);
            if (ok && v >= 1 && v <= 256) return v;
        }
        return 16;
    }();
    return n;
}

// How much faster than the pointer the preview walks, as a fraction. Matching
// the pointer's rate exactly holds the lag constant at whatever it already is;
// this is what closes it. Small on purpose -- a large value converges quickly
// and then overshoots into the pointer, which reads as the picture twitching.
constexpr double kScrubCatchUp = 0.25;

int scrubRearmMs() {
    static const int ms = [] {
        const QByteArray raw = qgetenv("TRACE_SCRUB_REARM_MS");
        if (!raw.isEmpty()) {
            bool ok = false;
            const int v = raw.toInt(&ok);
            if (ok && v >= 0 && v <= 200) return v;
        }
        return 0;
    }();
    return ms;
}

} // namespace

MainWindow::~MainWindow() {
    // Explicit, and before any member is destroyed. Cancellation is raised
    // first and the join is bounded by one checkpoint inside the GOP walk, so
    // quitting mid-drag on heavy media does not hang on a decode. The reverted
    // attempt joined a worker that could be anywhere in a walk with nothing to
    // interrupt it.
    //
    // Quitting mid-reverse goes through the same door. No landing decode: there
    // is nothing left to show it on.
    endShuttleRun(/*landExactly=*/false);
    reclaimDecoder();
    scrubWorker_.stop();
}

MainWindow::MainWindow() {
    setWindowTitle("Trace");
    setAcceptDrops(true);
    setupUi();
    setupSharedActions();
    setupMenus();
    setupTransportControls();
    // Last: every action it lists has to exist before it can point at one.
    setupShortcuts();

    // Open Recent (spec phase 11). Reads stored strings and builds the submenu
    // from them; NOTHING here touches the filesystem, so a list full of
    // disconnected LucidLink or UNC paths costs a launch exactly nothing. That
    // is the spec's "do not probe every path during application startup", and
    // it is a property of load() and rebuildRecentMenu() having no filesystem
    // call in them rather than a rule someone has to remember.
    recentFiles_.load();
    rebuildRecentMenu();

    // Spec phase 14. Applied here rather than only when the item is toggled, so
    // the stored preference is in force from the first frame -- a checked menu
    // item over a window that is not on top is exactly the kind of small
    // disagreement this pass exists to remove.
    if (alwaysOnTopAction_ && alwaysOnTopAction_->isChecked()) applyAlwaysOnTop(true);

    // The accessibility proxy tree over the composited transport (plan section
    // 19.7). Built for the OVERLAY only: in TRACE_TRANSPORT_BAR=1 mode the
    // transport is real QPushButtons and a real QSlider, which Qt already
    // exposes to UI Automation -- a proxy tree there would announce every
    // control twice.
    //
    // After setupTransportControls(), because it points at those actions.
    if (viewer_ && viewer_->overlayEnabled()) {
        OverlayAccessibility::Commands commands;
        commands.playPause = playPauseAction_;
        commands.rewind = rewindAction_;
        commands.fastForward = fastForwardAction_;
        overlayAccessibility_ =
            new OverlayAccessibility(viewer_, &viewer_->overlayModel(), commands);
    }

    connect(&playTimer_, &QTimer::timeout, this, [this]() {
        // GATE E: the timer is re-armed per wake against an absolute deadline,
        // so it does not free-run. EVERY exit path below has to re-arm or
        // playback stops dead -- hence a scope guard declared before the first
        // return rather than a call at the end. armNextPresent() is a no-op
        // when playTimer_ is inactive, so the stop paths inside stay correct.
        const auto armNext = qScopeGuard([this]() { armNextPresent(); });

        if (!frameSource_ || !frameSource_->canPlay()) return;
        // A tick delivered by the event pump that runs during a slow remote
        // read must not re-enter the decoder. Skipping it is correct: the
        // frame currently being decoded is the one that tick would have asked
        // for anyway, and the playback clock is not advanced here.
        if (storageBusy_) return;

        // Callback-to-callback period, measured at handler entry. Everything
        // not inside this handler (queued paint, backing-store flush, event
        // dispatch, idle wait for the next tick) is the difference between
        // this period and the handler duration recorded at the end.
        if (frameCycleClock_.isValid()) {
            lastPeriodMs_ = static_cast<double>(frameCycleClock_.nsecsElapsed()) / 1'000'000.0;
            lastOutsideMs_ = lastPeriodMs_ - lastHandlerMs_;
            ++cycleSamples_;
            const double cn = static_cast<double>(cycleSamples_);
            avgPeriodMs_ += (lastPeriodMs_ - avgPeriodMs_) / cn;
            avgOutsideMs_ += (lastOutsideMs_ - avgOutsideMs_) / cn;
            maxPeriodMs_ = std::max(maxPeriodMs_, lastPeriodMs_);
        }
        frameCycleClock_.restart();
        QElapsedTimer handlerTimer;
        handlerTimer.start();
        const auto recordHandler = qScopeGuard([this, &handlerTimer]() {
            lastHandlerMs_ = static_cast<double>(handlerTimer.nsecsElapsed()) / 1'000'000.0;
            const double hn = static_cast<double>(cycleSamples_ + 1);
            avgHandlerMs_ += (lastHandlerMs_ - avgHandlerMs_) / hn;
            // The count that identifies cause B. A handler that runs past the
            // frame budget is late immediately -- decode is synchronous and
            // there is no read-ahead absorbing it -- so this being non-zero is
            // the signature of per-frame cost overrun rather than of the tick
            // beat, which overruns nothing and simply skips an opportunity.
            //
            // Read from the member rather than captured: the budget is derived
            // from fps and speed further down this handler, and this guard runs
            // after that, so by the time it fires the value is this tick's.
            if (tickFrameDurationMs_ > 0.0) {
                ++handlerSamples_;
                if (lastHandlerMs_ > tickFrameDurationMs_) ++handlerOverBudget_;
                maxHandlerMs_ = std::max(maxHandlerMs_, lastHandlerMs_);
            }
        });

        // Clock-update accounting, measured between consecutive tick entries so
        // it covers everything in the gap -- the HUD refresh at the end of this
        // tick, and any refreshHud() a keypress triggered in between. The audio
        // control loop must be stepped exactly once per tick while it drives;
        // anything else means telemetry is moving the playhead.
        {
            const long long updates = audio_.clockUpdateCount();
            if (lastClockUpdateMark_ >= 0) {
                lastClockUpdatesPerTick_ = updates - lastClockUpdateMark_;
                maxClockUpdatesPerTick_ = std::max(maxClockUpdatesPerTick_,
                                                   lastClockUpdatesPerTick_);
            }
            lastClockUpdateMark_ = updates;
        }

        const auto playbackState = playback_.state();
        if (playbackState.mode != PlaybackMode::PlayingForward && playbackState.mode != PlaybackMode::PlayingReverse) {
            // Something stopped playback without going through a key handler.
            // The lease has to come back here too. Enumerating every path that
            // ends a run rather than testing the one the harness drives is the
            // section 29.2 lesson applied in advance.
            endShuttleRun(/*landExactly=*/true);
            playTimer_.stop();
            stopAudio();
            userPlayIntent_ = false;
            playbackClock_.invalidate();
            playbackAccumulatorMs_ = 0.0;
            return;
        }

        // One guard covering every way playback can stop being 1x forward:
        // J-K-L shuttle, reverse, a speed change mid-run. All of them go
        // through playback state, and the timer is still ticking here.
        if (audioDriving_ && !audioShouldDrive()) {
            stopAudio();
        }

        const int direction = playbackState.mode == PlaybackMode::PlayingReverse ? -1 : 1;
        // The reverse shuttle carries its speed in the STRIDE, not in the tick
        // rate: it presents one frame per source-frame period at every speed and
        // changes which frames those are. So its period is the source period,
        // undivided.
        //
        // Dividing it as well would ask for 30 presents per source frame period
        // at 30x -- 720 frames a second, which no decoder on this box reaches and
        // which the owner explicitly does not require. Holding the presentation
        // rate constant is also what makes the cadence identical at 1x and 30x,
        // which is the "stable, intentional" half of the goal.
        // A shuttle run presents one frame per `advance` frames of content, and
        // the content must move at exactly the commanded speed. So the period is
        // scaled by advance/stride: equal to the source period when they match
        // (advance == stride, the ordinary case), and stretched when the snap
        // makes one present cover a whole GOP.
        //
        // 1080p at 30x with a 48-frame GOP: 48/30 = 1.6 source periods per
        // present = 66.8ms = 15 presentations a second, at an exact 30x. That
        // is the owner's decision of 2026-08-10 -- accurate speed, stable ~15fps
        // presentation -- expressed as arithmetic rather than as a target.
        const double speed = shuttleRunActive_
            ? (static_cast<double>(std::max<long long>(1, shuttleStride_))
                   / static_cast<double>(std::max<long long>(1, shuttleAdvance_)))
            // kMinPlaybackSpeed, not 1.0. The clamp exists to stop a zero or a
            // negative dividing the period, and it read 1.0 for as long as 1x
            // was the slowest rate there was. Spec phase 14's 0.5x is below it,
            // and under the old clamp it would have been silently rounded UP to
            // 1x -- the menu would have ticked 0.5x and the file would have
            // played at normal speed, which is the kind of disagreement between
            // the label and the engine that section 11b.2 spent a session on.
            : std::max(trace::core::kMinPlaybackSpeed, std::abs(playbackState.speed));
        const double fps = std::max(1.0, frameSource_->fps());

        // GATE E: the frame period comes from the EXACT rational when the
        // container carries one. Everything else in this handler keeps using
        // fps(), and should -- but a schedule built on 24000/1001 rounded to a
        // double cannot be the reference for its own cadence error.
        int fpsNum = 0;
        int fpsDen = 0;
        const bool exactRate = frameSource_->fpsRational(fpsNum, fpsDen);
        const double frameDurationMs =
            exactRate ? (1000.0 * static_cast<double>(fpsDen)) / (static_cast<double>(fpsNum) * speed)
                      : 1000.0 / (fps * speed);
        // Published for the handler scope guard above, which needs the budget
        // this tick was actually working to.
        tickFrameDurationMs_ = frameDurationMs;

        // Establishes the timeline on the first tick of a run, and re-epochs it
        // if the speed changed. A no-op on every other tick.
        syncPresentTimeline(frameDurationMs);

        // How far past its armed deadline this wake landed. Measured before any
        // work, so it is the scheduler's error and not this frame's cost.
        if (presentTargetNs_ >= 0 && sessionClock_.isValid()) {
            presentSlotLatencyMs_ =
                static_cast<double>(sessionClock_.nsecsElapsed() - presentTargetNs_) / 1'000'000.0;
        }

        if (!playbackClock_.isValid()) {
            playbackClock_.start();
            playbackAccumulatorMs_ = 0.0;
        } else {
            // Nanoseconds, not restart(). QElapsedTimer::restart() returns whole
            // milliseconds and throws the remainder away, so the accumulator lost
            // an average of 0.5ms on every tick -- a systematic rate deficit
            // proportional to tick frequency, which is why it got worse as frame
            // rate rose. Measured before this change, against a predicted loss of
            // (ticks/sec * 0.5ms):
            //   4K 60fps, no audio:      62.1 ticks/s -> predict 96.9%, measured 96.4%
            //   1080p 24fps, no audio:   24.4 ticks/s -> predict 98.8%, measured 98.7%
            //   4K ProRes 4444, no audio:24.4 ticks/s -> predict 98.8%, measured 98.3%
            // Reading nsecsElapsed() and restarting loses only the few hundred
            // nanoseconds between the two calls (~0.0006%), and needs no extra
            // state: every existing start()/invalidate() site keeps working
            // unchanged because the reference is still the timer itself.
            //
            // Audio-mastered playback was never affected -- the audio clock
            // supplies position there, so this accumulator does not set the rate.
            const qint64 elapsedNs = playbackClock_.nsecsElapsed();
            playbackClock_.start();
            playbackAccumulatorMs_ += static_cast<double>(elapsedNs) / 1'000'000.0;
        }

        const bool isVideo = currentMedia_.has_value() && currentMedia_->kind == MediaKind::VideoFile;

        // Timer jitter: how far this tick landed from the requested interval.
        // Sampled before any presentation gating so it measures the scheduler
        // itself rather than the decision made from it.
        if (isVideo) {
            if (!schedulerTickClock_.isValid()) {
                schedulerTickClock_.start();
            } else {
                // Nanoseconds, not restart(): restart() returns whole
                // milliseconds, which is why this metric used to read as
                // integers. Sub-millisecond precision is the point now that
                // cadence is the thing being measured.
                const double tickDeltaMs =
                    static_cast<double>(schedulerTickClock_.nsecsElapsed()) / 1'000'000.0;
                schedulerTickClock_.start();

                // The reference is the FRAME PERIOD, not the armed interval.
                //
                // Under GATE E the timer is re-armed at the END of the handler,
                // so the armed interval excludes the handler's own duration
                // while tickDelta includes it: on 4444 (33ms handler, 41.67ms
                // period) that read `tick 9ms` and `jitter 34ms` and looked
                // like catastrophic scheduler error when the schedule was in
                // fact within 1.8ms of its deadline all run. Measuring the
                // wake-to-wake interval against the true period says what was
                // meant, and stays comparable with the pre-GATE-E numbers in
                // plan section 23.4, where the armed interval WAS the period.
                //
                // tickFrameDurationMs_ still holds the previous tick's budget
                // here -- it is recomputed further down. At a steady speed the
                // two are identical, and across a speed change one tick is
                // measured against the old period, which is correct: that is
                // the period the wake was actually scheduled under.
                const double referenceMs =
                    (deadlineScheduleEnabled() && tickFrameDurationMs_ > 0.0)
                        ? tickFrameDurationMs_
                        : static_cast<double>(schedulerIntervalMs_);
                lastTickJitterMs_ = tickDeltaMs - referenceMs;
                const double absJitter = std::abs(lastTickJitterMs_);
                ++schedulerTicks_;
                avgTickJitterMs_ += (absJitter - avgTickJitterMs_) / static_cast<double>(schedulerTicks_);
                maxTickJitterMs_ = std::max(maxTickJitterMs_, absJitter);
            }
        }

        int steps = static_cast<int>(std::floor(playbackAccumulatorMs_ / frameDurationMs));
        if (steps < 1) steps = 1;

        // Whether the audio clock is the authority for this tick. Resolved
        // before the accumulator gate because when audio drives, the
        // accumulator must not also get a vote -- see below.
        const bool audioActive = audioDriving_ && audio_.isPlaying()
                              && !audio_.ended() && audio_.clockReady();
        audioClockPriming_ = audioDriving_ && audio_.isPlaying()
                          && !audio_.ended() && !audio_.clockReady();

        if (isVideo) {
            // The short tick exists to land on each frame's due time, not to
            // present once per tick. Presenting per tick made the timer
            // interval the playback rate: 1000/24 rounds to a 42ms interval,
            // capping playback at 23.81fps no matter how fast decode is.
            // Retained as a guard: with the periodic timer at the frame
            // interval this is effectively never taken, but it keeps
            // presentation tied to the playback clock rather than to the tick.
            //
            // Bypassed while audio drives. Two clocks were deciding different
            // halves of the same question: this accumulator decided *when* to
            // present, the audio clock decided *which frame*. The tick is
            // floor(1000/fps) = 41ms against a 41.667ms frame, so roughly every
            // 62nd tick the accumulator came up short and returned here without
            // presenting -- and by the next tick the audio clock had moved on
            // two frames, so one was skipped. Holds and skips therefore arrived
            // in matched pairs at the beat frequency of the two clocks, which
            // is exactly the 1-2/sec residue that survived every attempt to
            // filter the clock itself. With audio driving, the audio clock is
            // the only scheduler: it decides both when and which.
            //
            // GATE E REMOVED THAT GATE ENTIRELY, for video and whether or not
            // audio drives. The wake IS the due time now: playTimer_ is armed
            // per frame at an absolute deadline computed from the exact source
            // rational, so a tick that arrives is by construction a frame that
            // is due. Gating it a second time against the wall-clock
            // accumulator would put a second opinion about WHEN back into the
            // path -- which is precisely the fault cd79d49 removed, and plan
            // section 24.3 forbids re-introducing it under a different name.
            //
            // The accumulator is still fed above. It is the position source for
            // the image-sequence branch below and it keeps a handover clean if
            // audio stops mid-run; it simply has no vote on when to present.
            //
            if (!deadlineScheduleEnabled() && !audioActive
                && playbackAccumulatorMs_ < frameDurationMs) {
                return;
            }

            // Presentation latency: how far past its armed deadline this wake
            // landed. The old expression measured the accumulator's surplus,
            // which under a deadline schedule is not an error term at all --
            // so the control keeps the old one and only the control.
            lastPresentLatencyMs_ = deadlineScheduleEnabled()
                ? presentSlotLatencyMs_
                : playbackAccumulatorMs_ - frameDurationMs;
            ++presentSamples_;
            avgPresentLatencyMs_ += (lastPresentLatencyMs_ - avgPresentLatencyMs_) / static_cast<double>(presentSamples_);
            maxPresentLatencyMs_ = std::max(maxPresentLatencyMs_, lastPresentLatencyMs_);

            // One frame per presentation unless the source cannot sustain its
            // native rate, in which case MEDIA TIME is held real-time and picture
            // is dropped (owner decision, 2026-08-13). See realtimeDropSteps():
            // it returns 1 whenever the run is on time, so a source that keeps up
            // is on exactly the path it was on before.
            steps = realtimeDropSteps(direction, playbackState, frameDurationMs);
            playbackAccumulatorMs_ -= steps * frameDurationMs;
            // Keep the residue: polling for the due time costs up to a tick of
            // latency per frame, and discarding it turns that into permanent
            // rate loss. Carrying it forward makes the next frame due
            // immediately, so the average converges on the true frame rate.
            // Capped so a long stall resumes at rate instead of fast-
            // forwarding through a large banked debt.
            const double maxBacklogMs = 4.0 * frameDurationMs;
            if (playbackAccumulatorMs_ > maxBacklogMs) playbackAccumulatorMs_ = maxBacklogMs;
            if (playbackAccumulatorMs_ < 0.0) playbackAccumulatorMs_ = 0.0;
        } else {
            playbackAccumulatorMs_ -= steps * frameDurationMs;
            if (playbackAccumulatorMs_ < 0.0) playbackAccumulatorMs_ = 0.0;
        }

        // Shuttle: the frame for this slot is already decoded and waiting. Pop
        // it, present it, return. Serves BOTH directions -- all reverse speeds,
        // and forward above 1x.
        //
        // Placed AFTER the timeline, jitter, accumulator and presentation-
        // latency bookkeeping so a shuttle run is measured by exactly the same
        // instruments as ordinary playback, and BEFORE the target arithmetic for
        // two reasons. The target of a shuttle run is the queue rather than a
        // sum; and loadCurrentFrame() calls reclaimDecoder(), which would revoke
        // the worker's lease on every single tick and turn the pipeline back into
        // a synchronous walk without changing a visible line of code.
        //
        // ORDINARY 1x FORWARD PLAYBACK NEVER ENTERS HERE. That path is audio
        // mastered and validated and is deliberately untouched: the shuttle is
        // started only for reverse, or for forward above 1x.
        if (shuttleRunActive_) {
            if (presentQueuedShuttleFrame()) {
                notePresentedPlaybackFrame(frameDurationMs);
            } else {
                // Starved: the pipeline has not produced this slot's frame yet.
                // HOLD -- do not decode on this thread. A starve is a cadence
                // event worth counting, not an excuse to take the work back.
                ++shuttleStarves_;
            }
            // The run is over when the arithmetic has run off the end of the
            // media -- the head going backward, the tail going forward -- and
            // everything it produced has been shown.
            //
            // Running off the end STOPS PLAYBACK. Ending the run without
            // stopping was a real bug and an instructive one: shuttleRunActive_
            // went false while the mode stayed Playing, so the next tick took
            // the ordinary synchronous path at the shuttle's speed -- period
            // 41.71/30 = 1.39ms -- and decoded on the UI thread as fast as it
            // could. The symptom was a cadence line reading `sched tick 1ms` on
            // a run that had presented perfectly, i.e. only in its tail.
            if (shuttleNextTarget_ < 0 && shuttleQueue_.empty() && !scrubWorker_.busy()) {
                const bool hitTail = shuttleDir_ > 0;
                // Captured before endShuttleRun clears them: a wrap restarts
                // the run at the same speed and direction from the far end, so
                // Loop at 10x keeps running at 10x rather than dropping to 1x.
                const int wrapDir = shuttleDir_;
                const int wrapStride = shuttleStride_;
                endShuttleRun(/*landExactly=*/true);

                // Loop applies to a SHUTTLE run too, not only to 1x playback. A
                // Loop that silently stopped applying above 1x would be a menu
                // item whose meaning depended on the rate.
                if (loopWrap(wrapDir)) {
                    startShuttleRun(wrapDir, wrapStride);
                    refreshHud(hitTail ? "FF loop" : "Reverse loop");
                    return;
                }

                playTimer_.stop();
                stopAudio();
                playback_.pause();
                userPlayIntent_ = false;
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
                // Forward off the tail is the end of the file, so Play restarts
                // it -- the same contract ordinary forward playback has.
                if (hitTail) {
                    playbackAtEnd_ = true;
                    playbackEndFrame_ = playback_.state().currentFrame;
                }
                syncPlaybackSpeedActions();
            }
            refreshHud(shuttleDir_ > 0 ? "FF" : "Reverse Play");
            return;
        }

        const long long beforeFrame = playbackState.currentFrame;
        long long unclampedTarget = beforeFrame + static_cast<long long>(direction) * steps;

        // Audio is the master clock while it plays. Taking the target frame
        // from the device clock rather than from a wall-clock accumulator is
        // what keeps picture locked to sound: the sound card's rate is the one
        // rate in the system that cannot be negotiated with, and it is also
        // what lifts the 23.81fps ceiling the 42ms tick imposed.
        // Not until the device is actually making sound: see clockReady(). The
        // wall-clock accumulator above already produced a correct target for
        // this tick, so the priming window costs nothing and is provably clean
        // (a no-audio run presents every frame with zero corrections).
        if (audioActive) {
            // The one and only place the audio control loop is stepped. Every
            // other reader (HUD, stats) peeks.
            const double audioSeconds = audio_.advanceClock();
            const double audioFramePos = audioSeconds * fps;
            const long long audioFrame = static_cast<long long>(std::llround(audioFramePos));

            lastAvSyncMs_ = (static_cast<double>(beforeFrame) - audioFramePos) * (1000.0 / fps);
            maxAvSyncMs_ = std::max(maxAvSyncMs_, std::abs(lastAvSyncMs_));

            // Watchdog. A clock that stops advancing must degrade to "audio out
            // of sync", never to "picture frozen" -- a stalled QAudioSink held
            // the playhead still indefinitely and looked like a hung app.
            // Picture keeps moving on the wall clock; only sync is lost.
            if (std::abs(audioSeconds - lastAudioClockS_) > 1e-6) {
                lastAudioClockS_ = audioSeconds;
                audioClockStall_.restart();
            } else if (audioClockStall_.isValid()
                       && audioClockStall_.elapsed() > kAudioStallMs) {
                audioClockStalled_ = true;
                audioDriving_ = false;
                statusBar()->showMessage(
                    "Audio clock stalled - playback continued without sync", 4000);
            }

            const long long delta = audioFrame - beforeFrame;
            if (delta <= 0) {
                // Sound has not reached the next frame yet. Hold the current
                // frame and take no decode step: requesting the same index in
                // Playback mode would advance the decoder, which is exactly the
                // frame-order bounce the linear-decode invariant exists to
                // prevent.
                ++audioRepeatedFrames_;
                refreshHud("Play");
                return;
            }

            // Bounded catch-up. Small forward jumps decode forward (see the
            // playback walk allowance in VideoDecoderFFmpeg); a larger jump
            // would force a seek and cost far more time than the drift it was
            // correcting, so cap it and let the remainder be caught next tick.
            //
            // THIS IS THE REAL-TIME DROP ON THE AUDIO CLOCK, and it predates the
            // owner decision of 2026-08-13 by a year: holding picture to the
            // device clock has always meant dropping frames the clock has run
            // past. It is counted into the same totals as the wall-clock path now,
            // so `drop` on the HUD is the whole story rather than half of it --
            // `audioSkippedFrames_` is kept beside it because a drop that is the
            // audio clock's doing and a drop that is cost overrun are different
            // conditions and the audio line is where the first one is diagnosed.
            constexpr long long kMaxCatchUpFrames = 3;
            const long long advance = std::min(delta, kMaxCatchUpFrames);
            if (advance > 1) {
                audioSkippedFrames_ += advance - 1;
                playbackDroppedFrames_ += advance - 1;
                ++playbackDropTicks_;
                maxDropRun_ = std::max(maxDropRun_, advance - 1);
            }
            unclampedTarget = beforeFrame + advance;
        }

        const long long minFrame = 0;
        const long long maxFrame = playbackState.maxFrame >= 0 ? playbackState.maxFrame : beforeFrame;
        const long long targetFrame = std::clamp(unclampedTarget, minFrame, maxFrame);

        // THE PREFETCH ANSWERS THIS SLOT, OR THE SLOT HOLDS. It never decodes
        // here.
        //
        // THE PLAYHEAD IS NOT MOVED BEFORE ASKING, and that is not a tidiness
        // choice -- moving it first is a runaway. The synchronous path can set
        // the frame up front because it then decodes it and reverts on failure;
        // the prefetch cannot, because a starve is not a failure and happens
        // every slot the decoder is slower than the clock. Measured on the 8K
        // plate with the playhead advanced before asking: the target ran ahead
        // at 24 fps while the pipeline supplied 20, so every frame that arrived
        // was already behind the target and was discarded on arrival --
        // `posted 94 | drop 93 | starve 146 | reseed 50` and ONE frame
        // presented in 6.14 seconds, 0.7% of real time against the synchronous
        // path's 53.6%.
        //
        // A starve therefore leaves the playhead exactly where it was, which is
        // the same thing an audio hold does a hundred lines above. The playhead
        // is set from the delivered frame's own index inside
        // presentQueuedPlaybackFrame().
        //
        // Placed AFTER the target arithmetic, which is untouched: the audio
        // clock or the deadline scheduler has already decided which frame and
        // when, and the queue only answers "do I have it". Deciding the target
        // from the queue instead would be two mechanisms each owning half of
        // "which frame, when" -- cd79d49, and plan section 24.3 forbids
        // re-introducing it under a different name.
        //
        // A starve HOLDS and does not take the decoder back. reclaimDecoder()
        // costs a revokeLease() wait of up to one cancellation checkpoint and
        // would turn the pipeline into a synchronous walk with a stall in front
        // of it. This is the decision the reverse shuttle already took, in the
        // same words: a starve is a cadence event worth counting, not an excuse
        // to take the work back.
        if (playbackPrefetchActive_) {
            if (presentQueuedPlaybackFrame(targetFrame)) {
                notePresentedPlaybackFrame(frameDurationMs);
            } else {
                ++pqStarves_;
                // Exhausted AND drained: this is the prefetch's form of the
                // second end-of-media site, the one only long-GOP media
                // reaches. Fall through to the shared end-of-media block by
                // pretending the target clamped, rather than writing a fourth
                // copy of stop-playback-and-maybe-loop.
                if ((playbackPrefetchExhausted_ || playbackPrefetchNext_ < 0)
                    && playbackQueue_.empty() && !scrubWorker_.busy()) {
                    if (direction > 0 && loopWrap(direction)) {
                        refreshHud("Play loop");
                        return;
                    }
                    playTimer_.stop();
                    stopAudio();
                    playback_.pause();
                    userPlayIntent_ = false;
                    playbackClock_.invalidate();
                    playbackAccumulatorMs_ = 0.0;
                    if (direction > 0) {
                        playbackAtEnd_ = true;
                        playbackEndFrame_ = playback_.state().currentFrame;
                    }
                    syncPlaybackSpeedActions();
                }
                refreshHud("Play");
                return;
            }
            if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
                prefetchNeighbors();
            }
            // Straight to the shared end-of-media check below, which the
            // synchronous path also reaches -- there is exactly one of those
            // and both paths use it.
            if (targetFrame == beforeFrame
                || (direction > 0 && targetFrame >= maxFrame)
                || (direction < 0 && targetFrame <= minFrame)) {
                const bool atEnd = (direction > 0 && targetFrame >= maxFrame)
                                || (direction < 0 && targetFrame <= minFrame);
                if (atEnd && loopWrap(direction)) {
                    refreshHud(direction > 0 ? "Play loop" : "Reverse loop");
                    return;
                }
                playTimer_.stop();
                stopAudio();
                playback_.pause();
                userPlayIntent_ = false;
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
                if (direction > 0 && targetFrame >= maxFrame) {
                    playbackAtEnd_ = true;
                    playbackEndFrame_ = targetFrame;
                }
                syncPlaybackSpeedActions();
            }
            refreshHud("Play");
            return;
        }

        playback_.setCurrentFrame(targetFrame);
        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, direction);
        QString error;
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Playback)) {
            playback_.setCurrentFrame(beforeFrame);
            // THE SECOND OF THE THREE END-OF-MEDIA SITES, and the one only
            // long-GOP media reaches -- the decoder is exhausted before the
            // frame count says it should be. Loop has to be answered here as
            // well or a file would loop or not depending on its codec.
            if (direction > 0 && loopWrap(direction)) {
                refreshHud("Play loop");
                return;
            }
            playTimer_.stop();
            stopAudio();
            playback_.pause();
            // Ran out of frames: there is nothing left to intend. A later scrub
            // release must not resurrect a run that ended by itself.
            userPlayIntent_ = false;
            playbackClock_.invalidate();
            playbackAccumulatorMs_ = 0.0;
            // The decoder is exhausted at the tail: this is the end of the file
            // as far as the viewer is concerned, even though the frame count
            // says there is more. Treat it as such so Play restarts.
            if (direction > 0) {
                playbackAtEnd_ = true;
                playbackEndFrame_ = playback_.state().currentFrame;
            }
            syncPlaybackSpeedActions();
            if (!error.isEmpty()) statusBar()->showMessage(error, 2000);
        } else {
            notePresentedPlaybackFrame(frameDurationMs);
            if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
                prefetchNeighbors();
            }
        }

        if (targetFrame == beforeFrame || (direction > 0 && targetFrame >= maxFrame) || (direction < 0 && targetFrame <= minFrame)) {
            // THE THIRD END-OF-MEDIA SITE: the target clamped to an end. This
            // is the one ordinary 1x playback reaches on well-formed media, and
            // therefore the one Loop is normally exercised through.
            //
            // `targetFrame == beforeFrame` alone is NOT an end -- it is also
            // what an audio hold looks like -- so the wrap is offered only when
            // an end was actually reached, or a paused-looking tick would
            // restart the file from the middle.
            const bool atEnd = (direction > 0 && targetFrame >= maxFrame)
                            || (direction < 0 && targetFrame <= minFrame);
            if (atEnd && loopWrap(direction)) {
                refreshHud(direction > 0 ? "Play loop" : "Reverse loop");
                return;
            }
            playTimer_.stop();
            stopAudio();
            playback_.pause();
            userPlayIntent_ = false;
            playbackClock_.invalidate();
            playbackAccumulatorMs_ = 0.0;
            if (direction > 0 && targetFrame >= maxFrame) {
                playbackAtEnd_ = true;
                playbackEndFrame_ = targetFrame;
            }
            syncPlaybackSpeedActions();
        }
        refreshHud(direction > 0 ? "Play" : "Reverse Play");

    });

    scrubTimer_.setSingleShot(true);
    // Explicit interval on every start() below: catch-up slices re-arm with
    // start(0), which would otherwise leave the interval at zero for the
    // coalescing path too.
    scrubTimer_.setInterval(kScrubCoalesceMs);
    connect(&scrubTimer_, &QTimer::timeout, this, [this]() {
        flushVideoScrub(false);
    });

    // Measures the UI thread from outside the work it is doing. A 1ms timer can
    // only fire when the event loop is running, so the interval between two
    // consecutive firings is exactly how long the thread was unavailable to
    // deliver a mouse move or a repaint. Started per drag, not left running.
    uiServiceTimer_.setSingleShot(false);
    uiServiceTimer_.setTimerType(Qt::PreciseTimer);
    uiServiceTimer_.setInterval(1);
    connect(&uiServiceTimer_, &QTimer::timeout, this, [this]() {
        const qint64 nowNs = uiServiceClock_.nsecsElapsed();
        if (uiServiceLastNs_ >= 0) {
            const double gapMs = static_cast<double>(nowNs - uiServiceLastNs_) / 1'000'000.0;
            uiServiceGapMaxMs_ = std::max(uiServiceGapMaxMs_, gapMs);
            uiServiceGapSumMs_ += gapMs;
            ++uiServiceSamples_;
            if (gapMs > kUiServiceGapMs) ++uiServiceGapsOver_;
        }
        uiServiceLastNs_ = nowNs;
    });

    // Clears the shuttle-rate indicator. Same 1200ms the transport bar's own
    // label has always used -- read from the bar rather than restated, so the
    // two surfaces cannot show the rate for different lengths of time.
    rateFlashTimer_.setSingleShot(true);
    rateFlashTimer_.setInterval(trace::ui::TransportBar::rateFlashMs());
    connect(&rateFlashTimer_, &QTimer::timeout, this, [this]() {
        rateFlashText_.clear();
        if (viewer_) viewer_->update();
    });

    // Reveal the floating transport once at startup. With the docked bar out of
    // the layout it is the only transport there is, so an app that opened with
    // it already faded would present the user with a black stage and no controls
    // until they happened to move the pointer.
    if (viewer_) viewer_->revealOverlay();

    statusBar()->showMessage("Ready");
    refreshHud("Idle");
}

void MainWindow::setupUi() {
    auto* central = new QWidget(this);
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    viewer_ = new trace::ui::ViewerWidget(central);
    transportBar_ = new trace::ui::TransportBar(central);
    overlay_ = new trace::ui::TransportOverlay(central);

    layout->addWidget(viewer_, 1);

    // SPEC PHASE 6: the floating transport replaces the docked bar, so the bar
    // comes out of the layout -- and the 76 logical pixels it occupied go to the
    // viewer, which moves the VIDEO RECT and therefore cache depth and every
    // stall figure (section 22.8, and the phase 2 measurement of `H`). Quote
    // `display` as well as `win WxH` for anything measured across this change.
    //
    // The BAR OBJECT STAYS ALIVE either way, and that is deliberate rather than
    // lazy. `timelineSlider_` is its child and is the entire scrub state
    // machine: the press/move/release the overlay drives, `isSliderDown()`,
    // `scrubJumpPending_`, the step 5.6 play-intent restore. Re-homing that into
    // the overlay would mean writing a second scrub path, which is the one thing
    // the OverlayHooks design exists to avoid -- so instead the overlay drives
    // the real slider and the slider simply is not on screen. A hidden QSlider
    // emits and accepts everything a visible one does.
    barIsDocked_ = !trace::render::OverlayModel::enabledByEnvironment();
    if (barIsDocked_) layout->addWidget(transportBar_, 0);
    else transportBar_->hide();

    // Dev diagnostics HUD sits below the transport. It carries the perf
    // readouts used for playback validation, so it stays until the playback
    // foundation is signed off.
    layout->addWidget(overlay_, 0);
    setCentralWidget(central);

    // TEMPORARY, env-gated, ships inert. Decides whether the child-HWND viewer
    // surface can host a floating interface before the GPU work commits to it.
    // See src/ui/OverlaySpike.h.
    if (const int spike = qgetenv("TRACE_OVERLAY_SPIKE").toInt(); spike > 0) {
        trace::ui::installOverlaySpike(viewer_, spike);
    }

    installOverlayHooks();
}

// Connects the renderer-composited overlay spike to the application.
//
// Every entry runs an action that already exists. The renderer owns where the
// controls are and which one the pointer is over; it owns no playback state and
// issues no command of its own -- which is why the overlay cannot drift out of
// step with the transport bar, the menu or the keyboard.
//
// Scrubbing is the clearest case: rather than seeking, it drives the real
// QSlider's press/move/release. The overlay therefore inherits the entire
// existing scrub path -- the drag shuttle, the exact landing, and the step 5.6
// play-state restore -- instead of reimplementing any part of it.
void MainWindow::installOverlayHooks() {
    if (!viewer_) return;

    trace::render::OverlayHooks hooks;
    hooks.playPause = [this]() { if (playPauseAction_) playPauseAction_->trigger(); };
    hooks.rewind = [this]() { if (rewindAction_) rewindAction_->trigger(); };
    hooks.fastForward = [this]() { if (fastForwardAction_) fastForwardAction_->trigger(); };

    hooks.setScrubbing = [this](bool down) {
        if (timelineSlider_) timelineSlider_->setSliderDown(down);
    };
    hooks.seekToFraction = [this](double fraction) {
        if (!timelineSlider_) return;
        const int maxFrame = timelineSlider_->maximum();
        timelineSlider_->setValue(static_cast<int>(std::lround(fraction * maxFrame)));
    };

    hooks.isPlaying = [this]() {
        const auto mode = playback_.state().mode;
        return mode == PlaybackMode::PlayingForward || mode == PlaybackMode::PlayingReverse;
    };
    hooks.positionFraction = [this]() {
        const auto st = playback_.state();
        if (st.maxFrame <= 0) return 0.0;
        return static_cast<double>(st.currentFrame) / static_cast<double>(st.maxFrame);
    };
    // The SAME transient the docked bar's label shows, not a second reading of
    // playback_.state().speed. The old lambda was permanently non-empty -- it
    // returned "1.0x" or "PAUSED" whatever was happening -- so the panel carried
    // a standing readout where the spec asks for a rate that appears on a
    // shuttle press and clears itself.
    hooks.rateText = [this]() { return rateFlashText_; };

    // Spec phase 6. The overlay checks the two conditions it can see for itself
    // (pointer over a control, timeline drag in progress); this answers for the
    // ones only the application knows. A popup menu or a tooltip is a top-level
    // window of its own, so `activePopupWidget` covers both, and a modal dialog
    // -- the file chooser -- counts for the same reason.
    hooks.holdVisible = [this]() {
        if (QApplication::activePopupWidget() || QApplication::activeModalWidget()) return true;
        // A child control holding keyboard focus. Every transport widget is
        // Qt::NoFocus today so this cannot fire yet; it is written now because
        // phase 7 adds the first text-entry control and this is the predicate it
        // will need, and because leaving it out would make the omission
        // invisible rather than deliberate.
        //
        // SCOPED TO THIS WINDOW'S OWN CHILDREN, AND SPEC PHASE 13 IS WHY.
        //
        // QApplication::focusWidget() is application-wide, so a MODELESS window
        // that takes focus -- the Movie Inspector is Trace's first -- satisfies
        // `focus != this && focus != viewer_` for as long as it is focused. The
        // transport would then be held revealed indefinitely, not because
        // anything about the video is being interacted with, but because another
        // window is. A user reading metadata is looking at the picture, and a
        // panel that never fades back off it is the wrong answer.
        //
        // Deliberate, and it is a holdVisible answer rather than a second
        // mechanism: the veto is what "an interaction is in progress" means, and
        // an interaction in a separate top-level window is not one with this
        // one. The modal branch above is untouched, so both Go To prompts still
        // hold exactly as they did at phase 7 -- and they should, because a
        // modal dialog blocks this window rather than sitting beside it.
        const QWidget* focus = QApplication::focusWidget();
        return focus != nullptr && focus != this && focus != viewer_ &&
               focus->window() == this;
    };
    hooks.setCursorHidden = [this](bool hidden) {
        // The overlay decides WHEN -- the same inactivity that fades the panel.
        // The spec hides the cursor in fullscreen only, so this decides WHETHER,
        // and a window that leaves fullscreen while hidden gets it back below.
        if (viewer_) viewer_->setCursorHidden(hidden && isFullScreen());
    };
    hooks.toggleFullscreen = [this]() { if (fullscreenAction_) fullscreenAction_->trigger(); };

    // Spec phase 8. The overlay gives a point in SURFACE DEVICE PIXELS, because
    // that is the only coordinate space it has; the host divides by the device
    // pixel ratio to reach Qt's logical widget coordinates and maps to the
    // screen. Doing that conversion here rather than in the model is what keeps
    // the model free of any notion of a screen -- the same split that lets one
    // OverlayModel serve two backends.
    hooks.shareMenu = [this](int x, int y) {
        if (!viewer_) return;
        const double dpr = viewer_->devicePixelRatioF();
        const QPoint local(static_cast<int>(x / dpr), static_cast<int>(y / dpr));
        showShareMenu(viewer_->mapToGlobal(local));
    };

    // Spec phase 15. Both go straight to the viewer, which owns the scale and
    // the pan; MainWindow is a pass-through here on purpose, because a press
    // that lands on the picture is not a transport command and nothing in the
    // playback state has an opinion about it.
    //
    // NO refreshHud() ON EVERY MOVE. A pan drag produces a mouse move per
    // pointer sample and the HUD line is built from a dozen counters -- putting
    // the instrument inside the gesture is phase 12's fifth stale-instrument
    // finding in reverse. The `zoom` field is rebuilt on the next refresh, and
    // the picture itself is the feedback that matters here.
    hooks.canPan = [this]() { return viewer_ && viewer_->canPan(); };
    hooks.panBy = [this](int dx, int dy) {
        if (viewer_) viewer_->panBy(QPointF(dx, dy));
    };

    hooks.requestRepaint = [this]() { if (viewer_) viewer_->update(); };

    viewer_->setOverlayHooks(hooks);
}

// The QActions reached by more than one surface.
//
// Every other transport command already had one (see setupTransportControls);
// these two did not. Fullscreen was four lines duplicated between the File menu
// and the transport button, and the HUD toggle was an inline case in
// keyPressEvent, so neither had a single source for its enabled state, its
// checked state or its shortcut.
//
// Both are added to the window itself, not only to a menu: a QAction's shortcut
// is only live while the action belongs to a widget in the active window, and
// the View/Window menus that will hold them do not exist until spec phase 13.
void MainWindow::setupSharedActions() {
    fullscreenAction_ = new QAction(tr("Toggle &Fullscreen"), this);
    fullscreenAction_->setCheckable(true);
    // F11 and Alt+Enter are the spec's bindings; Ctrl+Return is Trace's existing
    // one and is kept, because the spec's own rule is to preserve an existing
    // shortcut on conflict. Putting them on ONE action rather than on a second
    // handler is the whole point of promoting it.
    //
    // F11 is listed first deliberately: Qt advertises only the first sequence, in
    // the menu and in whatever a tooltip is written to say, so the order decides
    // what the user is told. The transport button's tooltip says F11, and a menu
    // that named a different key for the same command would be the kind of small
    // disagreement this phase exists to remove.
    //
    // Escape-exits-fullscreen, geometry save/restore and the monitor rule are
    // spec phase 6 (fullscreen consolidation), deliberately not here: they change
    // what the toggle DOES, and this phase only changes how many places define it.
    fullscreenAction_->setShortcuts({QKeySequence(Qt::Key_F11),
                                     QKeySequence(Qt::CTRL | Qt::Key_Return),
                                     QKeySequence(Qt::ALT | Qt::Key_Return)});
    connect(fullscreenAction_, &QAction::triggered, this, &MainWindow::toggleFullscreen);
    addAction(fullscreenAction_);

    // "Escape exits fullscreen" (spec phase 6). A SECOND SURFACE onto the same
    // command, not a second definition of it: it triggers the action above and
    // owns no window state of its own.
    //
    // It is a separate action rather than a fourth shortcut on that one because
    // Escape must only mean this WHILE FULLSCREEN -- and a disabled QAction does
    // not consume its shortcut, so Qt passes the key straight through when the
    // window is not fullscreen. That is the whole rule, expressed as enablement
    // rather than as a branch inside a handler that has already swallowed the
    // key. It also cannot go in the plain-key half of ShortcutTable, whose
    // dispatcher consumes unconditionally.
    exitFullscreenAction_ = new QAction(tr("Exit Fullscreen"), this);
    exitFullscreenAction_->setShortcut(QKeySequence(Qt::Key_Escape));
    exitFullscreenAction_->setEnabled(false);
    connect(exitFullscreenAction_, &QAction::triggered, this, &MainWindow::toggleFullscreen);
    addAction(exitFullscreenAction_);

    // Clear Recent Files (spec phase 11). Created here rather than inside
    // rebuildRecentMenu() because that function replaces the submenu's contents
    // wholesale, and an action rebuilt on every list change would be a new
    // object each time -- fine for the entries, which ARE the list, and wrong
    // for a command that is always the same command.
    //
    // No confirmation prompt. The list is a convenience with no content of its
    // own, clearing it destroys nothing, and a confirmation would be the third
    // dialog in a menu whose entire job is to save a trip through a file picker.
    clearRecentAction_ = new QAction(tr("&Clear Recent Files"), this);
    connect(clearRecentAction_, &QAction::triggered, this, [this]() {
        recentFiles_.clear();
        rebuildRecentMenu();
        statusBar()->showMessage(tr("Recent files cleared."), 2000);
    });

    // Movie Inspector (spec phase 13). Ctrl+I, and it goes on a QAction rather
    // than into ShortcutTable's dispatched half -- phase 3's rule: that
    // dispatcher matches on the key and IGNORES MODIFIERS, so every modifier'd
    // shortcut in Trace is action-owned and appears in the table as a
    // documentation row pointing at its action. Put in the dispatched half,
    // Ctrl+I would have been reachable by pressing plain I.
    //
    // Which is worth stating, because plain I used to exist: it toggled
    // viewState_.showInfo, nothing read the flag, and phase 2 deleted both. The
    // key is free and stays free -- this does not resurrect it.
    inspectorAction_ = new QAction(tr("Show Movie &Inspector"), this);
    inspectorAction_->setCheckable(true);
    inspectorAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_I));
    connect(inspectorAction_, &QAction::triggered, this, &MainWindow::toggleMovieInspector);
    addAction(inspectorAction_);

    // Single-shot and armed by events, never running on a schedule. See the
    // declaration for why the refresh is deferred at all rather than done where
    // the change happens.
    inspectorRefreshTimer_.setSingleShot(true);
    inspectorRefreshTimer_.setInterval(kInspectorRefreshMs);
    connect(&inspectorRefreshTimer_, &QTimer::timeout, this, &MainWindow::refreshInspector);

    // Section 20.4. Armed from WM_DPICHANGED, never on a schedule. See the
    // declaration for why it is deferred rather than done in the handler.
    dpiReshapeTimer_.setSingleShot(true);
    dpiReshapeTimer_.setInterval(kDpiReshapeMs);
    connect(&dpiReshapeTimer_, &QTimer::timeout, this, &MainWindow::reshapeAfterDpiChange);

    // Time Display (spec phase 7). Four mutually exclusive readouts in one
    // QActionGroup, so "which readout is showing" has one owner and the menu
    // cannot show two ticks. Every one of them goes through setReadoutMode --
    // the group decides which is checked, setReadoutMode decides whether the
    // mode is allowed at all.
    auto* readoutGroup = new QActionGroup(this);
    readoutGroup->setExclusive(true);

    const auto addReadout = [&](QAction*& slot, const QString& text,
                                trace::core::PrimaryReadoutMode mode) {
        slot = new QAction(text, this);
        slot->setCheckable(true);
        readoutGroup->addAction(slot);
        connect(slot, &QAction::triggered, this, [this, mode]() { setReadoutMode(mode); });
        addAction(slot);
    };
    addReadout(timeDisplayFrameAction_, tr("Frame &Count"), PrimaryReadoutMode::Frame);
    addReadout(timeDisplaySecondsAction_, tr("&Seconds"), PrimaryReadoutMode::Seconds);
    addReadout(timeDisplayElapsedAction_, tr("&Elapsed Time"), PrimaryReadoutMode::Elapsed);
    addReadout(timeDisplayTimecodeAction_, tr("SMPTE &Timecode"), PrimaryReadoutMode::Timecode);

    goToFrameAction_ = new QAction(tr("&Go to Frame..."), this);
    goToFrameAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_G));
    connect(goToFrameAction_, &QAction::triggered, this, &MainWindow::promptGoToFrame);
    addAction(goToFrameAction_);

    // "Timec&ode", not "&Timecode". SMPTE Timecode is three items above in the
    // same submenu and already owns T -- a collision that shipped at phase 7
    // and was found at phase 14 by warnOnDuplicateMnemonics() on its first run,
    // not by anyone using the menu. C, S, E and G are taken by the four items
    // around it, so this one takes O.
    goToTimecodeAction_ = new QAction(tr("Go to Timec&ode..."), this);
    goToTimecodeAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_G));
    connect(goToTimecodeAction_, &QAction::triggered, this, &MainWindow::promptGoToTimecode);
    addAction(goToTimecodeAction_);

    // Both jumps carry a MODIFIER, which puts them on the QAction half of the
    // keyboard contract by the rule phase 3 set: a modifier'd shortcut goes on
    // an action, because QAction is what resolves modifier ambiguity properly
    // and ShortcutTable's own dispatcher deliberately ignores modifiers.
    syncTimeDisplayActions();

    // Share (spec phase 8). Three actions, created here for the same reason
    // fullscreen was promoted at phase 2: the menu bar, the docked bar's button
    // and the composited overlay's button are three surfaces onto one command,
    // and the spec requires their enabled state, tooltips and accessibility
    // names to stay synchronized. One QAction is what makes that structural.
    //
    // The icons come from the approved package. copy-lucidlink is a NEUTRAL
    // CHAIN GLYPH and deliberately not a LucidLink brand mark.
    copyFilePathAction_ = new QAction(tr("&Copy File Path"), this);
    copyFilePathAction_->setIcon(trace::ui::TransportBar::loadIcon(QStringLiteral("copy-path")));
    connect(copyFilePathAction_, &QAction::triggered, this, &MainWindow::copyMediaFilePath);
    addAction(copyFilePathAction_);

    copyLucidLinkAction_ = new QAction(tr("Copy &LucidLink Link"), this);
    copyLucidLinkAction_->setIcon(
        trace::ui::TransportBar::loadIcon(QStringLiteral("copy-lucidlink")));
    // Connected as of spec phase 9. Phase 8 deliberately left it without a
    // handler, because a command that appears to exist and changes nothing is
    // the `showInfo` failure phase 2 deleted; the gate said no and the action
    // did nothing, consistently. It now runs the installed integration's own
    // copy-link command.
    connect(copyLucidLinkAction_, &QAction::triggered, this, &MainWindow::copyLucidLinkForMedia);
    addAction(copyLucidLinkAction_);

    showInExplorerAction_ = new QAction(tr("Show in File &Explorer"), this);
    showInExplorerAction_->setIcon(
        trace::ui::TransportBar::loadIcon(QStringLiteral("show-in-explorer")));
    connect(showInExplorerAction_, &QAction::triggered, this,
            &MainWindow::showMediaInFileExplorer);
    addAction(showInExplorerAction_);

    // One QMenu instance, popped by both transport surfaces and reused as the
    // menu bar's submenu. Two QMenus built from the same actions would still be
    // two things to keep in step -- the ordering, the separators, the icons --
    // and this phase exists partly to not create that.
    shareMenu_ = new QMenu(tr("&Share"), this);
    shareMenu_->addAction(copyFilePathAction_);
    shareMenu_->addAction(copyLucidLinkAction_);
    shareMenu_->addSeparator();
    shareMenu_->addAction(showInExplorerAction_);
    // Qt hides a disabled item's tooltip in a menu unless the menu is told to
    // show them, and the whole point of the Unavailable state is that the row
    // states WHY. Without this the design's tooltips would be written, shipped
    // and invisible.
    shareMenu_->setToolTipsVisible(true);

    syncShareActions();

    // View transforms (spec phase 10). TEMPORARY VIEWING STATE ONLY: these run
    // through `ViewerWidget::setViewTransform`, which reaches the renderer and
    // nothing else. No decoder call, no cache entry, no frame index and no
    // timing is touched, and no byte of the source is read differently -- which
    // is why rotating during playback costs a coordinate change and not a
    // re-decode.
    //
    // Rotation goes through `rotatedOnScreen`, not `quarterTurns + 1`: with a
    // flip in force an added turn would visibly rotate the picture the wrong
    // way. See ViewTransform.h.
    const auto addViewAction = [this](QAction*& slot, const QString& text,
                                      const QKeySequence& shortcut,
                                      std::function<void()> run) {
        slot = new QAction(text, this);
        if (!shortcut.isEmpty()) slot->setShortcut(shortcut);
        connect(slot, &QAction::triggered, this, std::move(run));
        addAction(slot);
    };

    addViewAction(rotateLeftAction_, tr("Rotate &Left"),
                  QKeySequence(Qt::CTRL | Qt::Key_L), [this]() {
        applyViewTransform(viewer_->viewTransform().rotatedOnScreen(-1), "Rotate Left");
    });
    addViewAction(rotateRightAction_, tr("Rotate &Right"),
                  QKeySequence(Qt::CTRL | Qt::Key_R), [this]() {
        applyViewTransform(viewer_->viewTransform().rotatedOnScreen(1), "Rotate Right");
    });

    // Checkable, because a flip is a state the user can see is on, unlike a
    // rotation which is its own evidence. Their checked state is written by
    // syncViewTransformActions from the transform in force, never by the
    // handler -- so Reset unticks them without needing to know they exist.
    addViewAction(flipHorizontalAction_, tr("Flip &Horizontal"), {}, [this]() {
        applyViewTransform(viewer_->viewTransform().withFlipH(), "Flip Horizontal");
    });
    flipHorizontalAction_->setCheckable(true);
    addViewAction(flipVerticalAction_, tr("Flip &Vertical"), {}, [this]() {
        applyViewTransform(viewer_->viewTransform().withFlipV(), "Flip Vertical");
    });
    flipVerticalAction_->setCheckable(true);

    // NO SHORTCUT, deliberately. The approved package's section 10 puts Reset
    // View Transform on Ctrl+0, and the interface spec's own Keyboard section
    // gives Ctrl+0 to Actual Size. The spec is the governing document and its
    // rule on conflict is to preserve the existing binding, so this phase
    // claims neither: Actual Size does not exist yet, and taking its key now
    // would have to be undone by whoever adds it. Ctrl+L and Ctrl+R above are
    // unclaimed in both documents, so they are safe to take.
    // "Rese&t", not "&Reset": Rotate Right already owns R in this menu, and two
    // items sharing a mnemonic makes the key CYCLE the highlight instead of
    // activating either -- a menu that quietly stops responding to its own
    // underlined letter.
    addViewAction(resetViewTransformAction_, tr("Rese&t View Transform"), {}, [this]() {
        applyViewTransform(trace::render::ViewTransform{}, "Reset View Transform");
    });

    syncViewTransformActions();

    // ---- spec phase 15: view scaling ----------------------------------------
    //
    // Four commands over ONE piece of state, which is why they are built
    // together and why three of them are the same call with a different
    // argument. `addViewAction` is reused verbatim: these are view commands in
    // exactly the sense the transforms are -- no decoder request, no cache
    // entry, no frame index and no timing changes with any of them.
    //
    // CTRL+0 IS CLAIMED HERE, AND THIS IS THE PHASE THE PROJECT SAID WOULD
    // CLAIM IT. The approved design package puts Ctrl+0 on Reset View
    // Transform and the interface spec puts it on Actual Size; the spec
    // governs, its conflict rule is to preserve the existing binding, and at
    // phase 10 neither existed so phase 10 took neither. Reset stays
    // shortcut-less.
    addViewAction(actualSizeAction_, tr("&Actual Size"),
                  QKeySequence(Qt::CTRL | Qt::Key_0), [this]() {
        if (viewer_) viewer_->setActualSize();
        syncViewScaleActions();
        refreshHud("Actual Size");
    });
    // NO SHORTCUT, for the phase 10 reason rather than an oversight: the spec's
    // Keyboard section names Ctrl+0 and Ctrl+plus/minus and nothing else, and a
    // key claimed here would have to be given up by whoever the spec later
    // assigns it to. The route back from a zoom is Ctrl+minus, which is bound.
    addViewAction(fitToWindowAction_, tr("Fit to &Window"), {}, [this]() {
        if (viewer_) viewer_->setFitToWindow();
        syncViewScaleActions();
        refreshHud("Fit to Window");
    });
    // TWO SEQUENCES, AND THE ORDER IS THE PHASE 2 RULE: Qt advertises only the
    // first in a menu, so the spec's own Ctrl++ is named and Ctrl+= sits behind
    // it. Ctrl+= is not decoration -- on a US layout the plus is a shifted key,
    // so the literal Ctrl++ is a three-finger chord and Ctrl+= is what people
    // actually press.
    addViewAction(zoomInAction_, tr("Zoom &In"), {}, [this]() {
        if (viewer_) viewer_->zoomStep(+1);
        syncViewScaleActions();
        refreshHud("Zoom In");
    });
    zoomInAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Plus),
                                 QKeySequence(Qt::CTRL | Qt::Key_Equal)});
    // "&Zoom Out", not "Zoom &Out": L&oop already owns O in the View menu, and
    // two items sharing a mnemonic makes the key CYCLE the highlight instead of
    // activating either. Found by warnOnDuplicateMnemonics() rather than by
    // inspection -- which is the guard phase 14 added doing the job it was
    // added for, on the first menu built after it.
    addViewAction(zoomOutAction_, tr("&Zoom Out"), {}, [this]() {
        if (viewer_) viewer_->zoomStep(-1);
        syncViewScaleActions();
        refreshHud("Zoom Out");
    });
    zoomOutAction_->setShortcuts({QKeySequence(Qt::CTRL | Qt::Key_Minus),
                                  QKeySequence(Qt::CTRL | Qt::Key_Underscore)});
    // Checkable so the menu says which of the two named states is in force.
    // Written by syncViewScaleActions from the viewer, never by the handler
    // that set it -- the same rule the flips and the speed rungs follow, and
    // the reason a zoom reached by Ctrl+minus still unticks Actual Size.
    actualSizeAction_->setCheckable(true);
    fitToWindowAction_->setCheckable(true);
    syncViewScaleActions();

    // The dev diagnostics HUD. Owner request, 2026-08-10: a quick way to put the
    // instrument away while judging feel, and back for measuring.
    //
    // H rather than I. `I` was bound to viewState_.showInfo, a flag nothing read
    // -- so it repainted and did nothing -- and the spec claims Ctrl+I for the
    // Movie Inspector, which would have left plain `I` as a second,
    // differently-scoped info toggle. The dead flag is deleted rather than
    // wired up; there is one HUD and it is this one. Return is kept as a second
    // binding because it is what the shortcut has always been.
    toggleHudAction_ = new QAction(tr("Show &Diagnostics HUD"), this);
    toggleHudAction_->setCheckable(true);
    toggleHudAction_->setChecked(viewState_.showHud);
    toggleHudAction_->setShortcuts({QKeySequence(Qt::Key_H),
                                    QKeySequence(Qt::Key_Return),
                                    QKeySequence(Qt::Key_Enter)});
    connect(toggleHudAction_, &QAction::triggered, this, &MainWindow::setHudVisible);
    addAction(toggleHudAction_);

    // ---- spec phase 14 ------------------------------------------------------

    // NO SHORTCUT, deliberately. Ctrl+M is the macOS convention and Windows has
    // none, the spec's Keyboard section does not ask for one, and `M` is
    // already Mute -- a table-dispatched row, and that dispatcher MATCHES ON
    // THE KEY AND IGNORES MODIFIERS. Qt would resolve Ctrl+M to this action
    // before keyPressEvent was reached so the two would not actually collide,
    // but a mute key and a minimize key one modifier apart is a trap to walk
    // into rather than to document. The menu item is the surface.
    minimizeAction_ = new QAction(tr("Mi&nimize"), this);
    connect(minimizeAction_, &QAction::triggered, this, &QWidget::showMinimized);
    addAction(minimizeAction_);

    // ONE action for both directions, with its TEXT following the window state,
    // because the spec names one item ("Maximize/Restore") and two would need
    // one of them greyed at all times. The text is written from what the window
    // manager actually did, in changeEvent, never from what this handler asked
    // for -- phase 6's rule for fullscreen, and for the same reason: a maximize
    // Windows declines would otherwise leave the menu claiming it happened.
    maximizeRestoreAction_ = new QAction(tr("Ma&ximize"), this);
    connect(maximizeRestoreAction_, &QAction::triggered, this, [this]() {
        if (isMaximized()) showNormal();
        else showMaximized();
    });
    addAction(maximizeRestoreAction_);

    // ALWAYS ON TOP GOES THROUGH SetWindowPos, NOT setWindowFlag, and that is a
    // correctness requirement rather than a preference. Changing a top-level
    // widget's window flags makes Qt destroy and recreate the native window, and
    // the D3D11 swapchain's surface is a child HWND created once in
    // initialize() from the viewer's winId() -- so the obvious implementation
    // would orphan the surface the picture is presented into, on the default
    // renderer, the first time anyone ticked the box.
    alwaysOnTopAction_ = new QAction(tr("Always on &Top"), this);
    alwaysOnTopAction_->setCheckable(true);
    alwaysOnTopAction_->setChecked(
        trace::app::settings().value(QLatin1String(kAlwaysOnTopKey), false).toBool());
    connect(alwaysOnTopAction_, &QAction::toggled, this, [this](bool on) {
        trace::app::settings().setValue(QLatin1String(kAlwaysOnTopKey), on);
        applyAlwaysOnTop(on);
        refreshHud(on ? "Always on top" : "Not always on top");
    });
    addAction(alwaysOnTopAction_);

    // Loop. Persisted, because it is a review preference rather than a property
    // of the media -- someone checking a cycling animation wants it to survive
    // opening the next version of the same shot.
    // "L&oop", not "&Loop": the View menu already gives L to Lock Window to
    // Media Aspect Ratio. Two items sharing a mnemonic makes the key CYCLE the
    // highlight instead of activating either -- phase 10 hit this once and
    // named it; phase 14 built a View menu that hit it twice. See
    // warnOnDuplicateMnemonics(), which is why it will not happen a fourth time.
    loopAction_ = new QAction(tr("L&oop"), this);
    loopAction_->setCheckable(true);
    loopAction_->setChecked(
        trace::app::settings().value(QLatin1String(kLoopKey), false).toBool());
    loopEnabled_ = loopAction_->isChecked();
    connect(loopAction_, &QAction::toggled, this, [this](bool on) {
        loopEnabled_ = on;
        trace::app::settings().setValue(QLatin1String(kLoopKey), on);
        // Turning Loop ON at the end of a file does NOT restart it. Loop
        // decides what happens when playback REACHES the end, and the playhead
        // is only ever moved by an explicit request -- the same rule that keeps
        // the Play-at-end rewind inside the Play action rather than in the tick.
        refreshHud(on ? "Loop on" : "Loop off");
    });
    addAction(loopAction_);

    closeMediaAction_ = new QAction(tr("&Close Media"), this);
    closeMediaAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_W));
    connect(closeMediaAction_, &QAction::triggered, this, &MainWindow::closeMedia);
    addAction(closeMediaAction_);

    copyFrameAction_ = new QAction(tr("&Copy Current Frame"), this);
    copyFrameAction_->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_C));
    connect(copyFrameAction_, &QAction::triggered, this, &MainWindow::copyCurrentFrame);
    addAction(copyFrameAction_);

    keyboardShortcutsAction_ = new QAction(tr("&Keyboard Shortcuts"), this);
    connect(keyboardShortcutsAction_, &QAction::triggered,
            this, &MainWindow::showKeyboardShortcuts);
    addAction(keyboardShortcutsAction_);

    // TRACE HELP HAS REAL CONTENT AND IS NOT A SECOND SURFACE ONTO ABOUT.
    // Pointing it at the About box, or at the Keyboard Shortcuts window, would
    // be two menu items sharing one window -- a stub wearing a menu entry,
    // which is the thing Check for Updates was omitted to avoid. There is no
    // help site to link to (the repository is private), so the help is written
    // here: what Trace is for, and the four things about it that are not
    // guessable from the interface.
    traceHelpAction_ = new QAction(tr("&Trace Help"), this);
    connect(traceHelpAction_, &QAction::triggered, this, &MainWindow::showTraceHelp);

    // REPORT AN ISSUE IS A PRE-FILLED mailto, NOT A LINK INTO THE REPOSITORY.
    // Owner decision, 2026-08-11: the GitHub repo is private, so a link there
    // works for one person and is a dead end for every other tester. The body
    // carries the build identity because the first question about any report is
    // which build produced it, and a tester should not have to go and find it.
    reportIssueAction_ = new QAction(tr("&Report an Issue"), this);
    connect(reportIssueAction_, &QAction::triggered, this, [this]() {
        QUrl url(QStringLiteral("mailto:") + QLatin1String(kIssueEmail));
        QUrlQuery query;
        query.addQueryItem(QStringLiteral("subject"),
                           tr("Trace alpha - issue report"));
        query.addQueryItem(QStringLiteral("body"),
                           tr("What happened:\n\n\nWhat you expected:\n\n\n"
                              "Media (format, resolution, frame rate):\n\n\n"
                              "--- please leave the lines below ---\n%1")
                               .arg(buildIdentity()));
        url.setQuery(query);
        QDesktopServices::openUrl(url);
    });

    aboutAction_ = new QAction(tr("&About Trace"), this);
    connect(aboutAction_, &QAction::triggered, this, &MainWindow::showAboutDialog);
}

// Trace's complete keyboard contract, in one place.
//
// Spec phase 3 asked for the shuttle and stepping CONTRACTS, and this is the
// half of it that is about who owns a key. What was here before was a flat
// switch in keyPressEvent, which works fine as a dispatcher and is useless as a
// source: spec phase 13 has to render a Keyboard Shortcuts window, and a switch
// cannot be enumerated, printed, grouped or grepped for a conflict. Extending it
// would have meant a second, hand-written list of the same keys at phase 13 --
// two things to keep in agreement, which is exactly what phase 2 spent its
// effort removing from the fullscreen command.
//
// THE TABLE IS COMPLETE AND THE DISPATCHER IS NOT, and that separation is the
// design. Rows carrying a QAction are documentation only, because Qt dispatched
// them before keyPressEvent was ever reached; they point at the action instead
// of copying its keys, so they cannot go stale when a binding changes. Rows
// carrying a handler are the ones this window still dispatches itself.
//
// Behaviour is unchanged by construction: the dispatcher matches on the key and
// ignores modifiers, which is what a `switch (event->key())` did.
void MainWindow::setupShortcuts() {
    using trace::app::ShortcutGroup;

    shortcuts_.addAction(ShortcutGroup::File, openAction_);
    shortcuts_.addAction(ShortcutGroup::View, fullscreenAction_);
    shortcuts_.addAction(ShortcutGroup::View, exitFullscreenAction_);
    shortcuts_.addAction(ShortcutGroup::View, toggleHudAction_);
    // Both carry a modifier, so Qt dispatches them and these rows are
    // documentation -- which is what makes the table the complete keyboard
    // contract phase 13 has to render, rather than only the part this window
    // happens to dispatch itself.
    shortcuts_.addAction(ShortcutGroup::View, goToFrameAction_);
    shortcuts_.addAction(ShortcutGroup::View, goToTimecodeAction_);
    // Spec phase 10. Modifier'd, so Qt dispatches them and these are
    // documentation rows -- and they point AT the action rather than copying its
    // key, so a rebinding cannot leave the table stale. The three view-transform
    // actions with no shortcut are deliberately absent: the table is the
    // KEYBOARD contract, and a row with no key in it would be a menu listing.
    shortcuts_.addAction(ShortcutGroup::View, rotateLeftAction_);
    shortcuts_.addAction(ShortcutGroup::View, rotateRightAction_);
    // Spec phase 13, and a documentation row for the same reason: Ctrl+I carries
    // a modifier, so Qt dispatches it and this row exists so the Keyboard
    // Shortcuts window phase 14 renders is the COMPLETE contract rather than the
    // part keyPressEvent happens to own.
    shortcuts_.addAction(ShortcutGroup::View, inspectorAction_);

    // Spec phase 14, all documentation rows for the same reason as the block
    // above: every one carries a modifier, so Qt dispatches it and the row
    // exists so the Keyboard Shortcuts window this phase renders is the
    // COMPLETE contract. They point at their action rather than copying its
    // key, so a rebinding cannot leave this window stale -- which matters more
    // now than it ever has, since this window is the only place the contract is
    // read out loud.
    shortcuts_.addAction(ShortcutGroup::File, closeMediaAction_);
    shortcuts_.addAction(ShortcutGroup::View, copyFrameAction_);
    // Spec phase 15, documentation rows for the same reason. Fit to Window is
    // deliberately absent: it has no shortcut, and the table is the KEYBOARD
    // contract -- the same rule that keeps Minimize and the three shortcut-less
    // view transforms out of it.
    shortcuts_.addAction(ShortcutGroup::View, actualSizeAction_);
    shortcuts_.addAction(ShortcutGroup::View, zoomInAction_);
    shortcuts_.addAction(ShortcutGroup::View, zoomOutAction_);
    // Minimize is NOT listed: it has no shortcut, and the table is the KEYBOARD
    // contract. A row with no key in it would be a menu listing -- the same
    // rule that keeps the three shortcut-less view transforms out of it.

    shortcuts_.addKey(ShortcutGroup::Playback, Qt::Key_Space, tr("Play / Pause"),
                      [this]() { togglePlayPause(); refreshHud("Space"); });
    shortcuts_.addKey(ShortcutGroup::Playback, Qt::Key_M, tr("Mute"), [this]() {
        audio_.setMuted(!audio_.isMuted());
        refreshHud(audio_.isMuted() ? "Mute" : "Unmute");
    });

    // Frame stepping is KEYBOARD-ONLY as of spec phase 5, which is what the spec
    // asks for: the arrows are the only surface that reaches these two actions
    // now, because both buttons that used to are shuttle controls.
    //
    // The commands themselves are untouched by either phase, which is what the
    // spec's "do not delete the underlying exact-frame-step commands" requires,
    // and they are untouched precisely because phase 3 made the button and the
    // key trigger ONE action rather than two near-copies.
    shortcuts_.addKey(ShortcutGroup::Stepping, Qt::Key_Left, tr("Step one frame back"),
                      [this]() { prevFrameAction_->trigger(); });
    shortcuts_.addKey(ShortcutGroup::Stepping, Qt::Key_Right, tr("Step one frame forward"),
                      [this]() { nextFrameAction_->trigger(); });

    // AtOneX is the KEYBOARD convention and is now written here literally. Until
    // spec phase 5 it came through a shuttleEntryConvention() helper that
    // TRACE_SHUTTLE_ENTRY=2x could flip, because that was the only way to run
    // the buttons' 2x entry before the buttons existed. Both buttons exist now
    // and pass AtTwoX as an argument, so the knob is redundant and left with the
    // phase that made it so.
    shortcuts_.addKey(ShortcutGroup::Shuttle, Qt::Key_J,
                      tr("Rewind — 1x, 2x, 5x, 10x, 30x"), [this]() {
        startShuttle(-1, trace::core::ShuttleEntry::AtOneX);
        refreshHud("J");
    });
    shortcuts_.addKey(ShortcutGroup::Shuttle, Qt::Key_K, tr("Stop"), [this]() {
        // Before pause(): the lease has to come back and the exact landing has
        // to happen while the run still knows which frame was on screen.
        endShuttleRun(/*landExactly=*/true);
        playback_.pause();
        userPlayIntent_ = false;
        if (playTimer_.isActive()) {
            playTimer_.stop();
            stopAudio();
            playbackClock_.invalidate();
            playbackAccumulatorMs_ = 0.0;
        }
        refreshHud("K");
    });
    shortcuts_.addKey(ShortcutGroup::Shuttle, Qt::Key_L,
                      tr("Fast-forward — 1x, 2x, 5x, 10x, 30x"), [this]() {
        startShuttle(1, trace::core::ShuttleEntry::AtOneX);
        refreshHud("L");
    });

    // All four go through setReadoutMode, which is where SMPTE is refused when
    // the source has none. Writing viewState_.readoutMode from a key would be a
    // second way in, and the gate would then hold in the menu and not on the
    // keyboard.
    shortcuts_.addKey(ShortcutGroup::View, Qt::Key_F, tr("Readout: frame count"),
                      [this]() { setReadoutMode(PrimaryReadoutMode::Frame); });
    shortcuts_.addKey(ShortcutGroup::View, Qt::Key_S, tr("Readout: seconds"),
                      [this]() { setReadoutMode(PrimaryReadoutMode::Seconds); });
    shortcuts_.addKey(ShortcutGroup::View, Qt::Key_E, tr("Readout: elapsed time"),
                      [this]() { setReadoutMode(PrimaryReadoutMode::Elapsed); });
    shortcuts_.addKey(ShortcutGroup::View, Qt::Key_T, tr("Readout: source timecode"),
                      [this]() { setReadoutMode(PrimaryReadoutMode::Timecode); });
}

// Single-key shortcuts and text entry: there is NO text-entry control anywhere
// in Trace today (phase 1 audit section 4), so the spec's "must not fire while
// focus is inside a text-entry control" has nothing to guard yet. Qt's own
// mechanism covers it when there is: QLineEdit and friends accept
// QEvent::ShortcutOverride for printable keys, which suppresses the shortcut and
// delivers the keystroke to the widget. That is untested here because it is
// untestable here -- Go to Frame and Go to Timecode create the first text field,
// at spec phase 7, and verifying `H` does not eat a digit belongs with them.
// Spec phase 6 completes what phase 2 started. Phase 2 made this the ONE place
// the window's fullscreen state changes; this adds what the toggle has to DO --
// geometry preserved and restored, maximize kept distinct from fullscreen, and
// the monitor holding the active window.
void MainWindow::toggleFullscreen() {
    const bool entering = !isFullScreen();
    if (entering) {
        // Captured BEFORE the state change, because Qt's own restore is not
        // something to rely on across a window manager: saveGeometry() records
        // the normal-state rectangle and the screen it was on, which is also
        // what puts the window back on the right monitor afterwards.
        //
        // The maximized bit is recorded SEPARATELY. "Do not confuse fullscreen
        // with maximize" cuts both ways: a maximized window that goes fullscreen
        // must come back maximized, not restored to its pre-maximize rectangle.
        preFullscreenGeometry_ = saveGeometry();
        preFullscreenMaximized_ = isMaximized();
    }

    // XOR on the window state rather than showFullScreen()/showNormal(): the
    // other state bits have to survive the round trip, and this is the
    // expression that has been shipping. Qt takes the window fullscreen on the
    // screen it is currently on, which is the spec's monitor rule -- note that
    // the multi-monitor half of it is NOT testable on this box (section 20.4:
    // one display, and Parsec replaces it rather than adding one).
    setWindowState(windowState() ^ Qt::WindowFullScreen);

    if (!entering) {
        // Clear the fullscreen bit explicitly before restoring, or
        // restoreGeometry() applies a rectangle to a window still in a state
        // that ignores it.
        setWindowState(windowState() & ~Qt::WindowFullScreen);
        if (!preFullscreenGeometry_.isEmpty()) restoreGeometry(preFullscreenGeometry_);
        if (preFullscreenMaximized_) setWindowState(windowState() | Qt::WindowMaximized);
    }

    // Read back what actually happened rather than trusting the toggle. A
    // checkable action that flipped its own tick before this handler ran would
    // otherwise be able to disagree with the window.
    viewState_.fullscreen = isFullScreen();
    if (fullscreenAction_) fullscreenAction_->setChecked(viewState_.fullscreen);
    // The one place Escape's meaning is decided. Disabled, the shortcut is not
    // consumed at all and Escape keeps whatever meaning the rest of the app
    // gives it.
    if (exitFullscreenAction_) exitFullscreenAction_->setEnabled(viewState_.fullscreen);
    if (transportBar_) transportBar_->setFullscreen(viewState_.fullscreen);
    // Leaving fullscreen must give the pointer back unconditionally: the
    // cursor is only ever hidden in fullscreen, so a window that exits while
    // hidden would otherwise keep an invisible pointer over its own titlebar
    // until the next idle cycle.
    if (viewer_ && !viewState_.fullscreen) viewer_->setCursorHidden(false);
    // Changing what the window looks like is interaction, so the transport comes
    // back -- and in fullscreen with no docked bar it is the only transport
    // there is.
    if (viewer_) viewer_->revealOverlay();
    refreshHud("Fullscreen");
}

void MainWindow::setHudVisible(bool visible) {
    viewState_.showHud = visible;
    if (toggleHudAction_) toggleHudAction_->setChecked(visible);
    if (overlay_) overlay_->setVisible(visible);
    // The viewer takes the HUD's height when it goes away, so scrub previews get
    // bigger, so fewer of them fit the byte-budgeted reverse cache. Stall counts
    // move purely from pressing H: measured on the same 4K H.264 reversal drag,
    // `stalls 70 of 370` with the HUD shown and `127 of 450` with it hidden.
    // The section 22.8 effect, and not a regression.
    //
    // AND THE USUAL SAFEGUARD DOES NOT CATCH IT. The rule is to quote `win WxH`
    // with every scrub number, but the WINDOW does not change size here -- it
    // read 1280x843 in both runs. What changes is the VIDEO RECT, which the HUD
    // reports as `display`: 640x360 shown, 1280x720 hidden. Quote `display` too
    // when a number is taken with the HUD toggled. `hitch` read 1 either way,
    // which is again why it is the figure to compare across sessions.
    refreshHud(visible ? "HUD On" : "HUD Off");
}

// The one place the time readout changes, and the one place SMPTE is refused.
//
// A mode the source cannot support is DECLINED WITH A REASON rather than
// accepted and quietly rendered as something else. Accepting it would put the
// app back in the state phase 7 exists to leave: a readout labelled `Timecode:`
// showing a value the source never stated.
void MainWindow::setReadoutMode(trace::core::PrimaryReadoutMode mode) {
    if (mode == PrimaryReadoutMode::Timecode && !hasSourceTimecode_) {
        refreshHud("Timecode: source carries none");
        syncTimeDisplayActions();
        return;
    }
    viewState_.readoutMode = mode;
    syncTimeDisplayActions();
    switch (mode) {
        case PrimaryReadoutMode::Frame:    refreshHud("Readout: Frame"); break;
        case PrimaryReadoutMode::Seconds:  refreshHud("Readout: Seconds"); break;
        case PrimaryReadoutMode::Elapsed:  refreshHud("Readout: Elapsed"); break;
        case PrimaryReadoutMode::Timecode: refreshHud("Readout: Timecode"); break;
    }
}

void MainWindow::syncTimeDisplayActions() {
    const auto m = viewState_.readoutMode;
    if (timeDisplayFrameAction_) timeDisplayFrameAction_->setChecked(m == PrimaryReadoutMode::Frame);
    if (timeDisplaySecondsAction_) timeDisplaySecondsAction_->setChecked(m == PrimaryReadoutMode::Seconds);
    if (timeDisplayElapsedAction_) timeDisplayElapsedAction_->setChecked(m == PrimaryReadoutMode::Elapsed);
    if (timeDisplayTimecodeAction_) {
        timeDisplayTimecodeAction_->setChecked(m == PrimaryReadoutMode::Timecode);
        // "Enable only when valid timecode exists in the source." Disabled, the
        // item is visibly unavailable rather than absent, which is the honest
        // report: this file has no timecode, not this build has no feature.
        timeDisplayTimecodeAction_->setEnabled(hasSourceTimecode_);
    }
    if (goToTimecodeAction_) goToTimecodeAction_->setEnabled(hasSourceTimecode_);
    if (goToFrameAction_) goToFrameAction_->setEnabled(frameSource_ && frameSource_->maxFrame() >= 0);
}

// Parsed ONCE per media open, not per HUD refresh. The HUD rebuilds several
// times a second during a drag and re-parsing a timecode string there would be
// per-frame work for a value that cannot change while the file is open.
void MainWindow::refreshSourceTimecode() {
    hasSourceTimecode_ = false;
    sourceTimecodeDropFrame_ = false;
    sourceTimecodeStartFrames_ = 0;
    timecodeFpsNum_ = 24;
    timecodeFpsDen_ = 1;

    if (frameSource_) {
        int num = 0, den = 0;
        if (frameSource_->fpsRational(num, den) && num > 0 && den > 0) {
            timecodeFpsNum_ = num;
            timecodeFpsDen_ = den;
        } else {
            // No container rational. Fall back to the double, which is what
            // every other consumer uses -- but keep it as a ratio so the
            // timecode arithmetic has one code path rather than two.
            timecodeFpsNum_ = static_cast<int>(std::lround(frameSource_->fps() * 1000.0));
            timecodeFpsDen_ = 1000;
            if (timecodeFpsNum_ <= 0) { timecodeFpsNum_ = 24; timecodeFpsDen_ = 1; }
        }

        QString start;
        bool drop = false;
        if (frameSource_->sourceTimecode(start, drop)) {
            trace::core::TimeFormat::Timecode parsed;
            if (trace::core::TimeFormat::parseTimecode(start, parsed)) {
                parsed.dropFrame = drop;
                sourceTimecodeStartFrames_ =
                    trace::core::TimeFormat::timecodeToFrames(parsed, timecodeFpsNum_, timecodeFpsDen_);
                sourceTimecodeDropFrame_ = drop;
                hasSourceTimecode_ = true;
            }
        }
    }

    // New media with no timecode must not leave the readout claiming one. This
    // is the case the mode gate cannot catch, because nothing was selected --
    // the mode was already set when the file changed underneath it.
    if (!hasSourceTimecode_ && viewState_.readoutMode == PrimaryReadoutMode::Timecode) {
        viewState_.readoutMode = PrimaryReadoutMode::Elapsed;
    }
    syncTimeDisplayActions();
}

// Spec phase 8. Evaluated ONCE per media open, for the same reason the timecode
// above is parsed once: the gate asks MediaIoSource to classify the volume,
// which issues real Win32 volume queries, and the spec's performance list
// forbids filesystem probing in paint or timeline updates outright.
void MainWindow::refreshShareState() {
    QString path;
    bool fileBacked = false;
    if (currentMedia_.has_value()) {
        path = QString::fromStdString(currentMedia_->path);
        // Every media kind Trace opens today is a file on disk -- a video file,
        // a still, or one frame of a numbered sequence. `fileBacked` is asked as
        // a question about the MediaItem rather than assumed from the path so
        // that a future non-file source (a capture device, a URL) declines both
        // path commands by construction instead of by being remembered.
        fileBacked = !path.isEmpty();
    }
    // The integration answer is per-file and expensive, so it is cached against
    // the path it was obtained for. A path change invalidates it and starts a
    // new probe; re-entering with the same path (which is what opening the Share
    // menu does) reuses it and costs nothing.
    if (path != lucidProbePath_) {
        lucidProbePath_ = path;
        lucidState_ = trace::app::LucidIntegrationState{};
        const auto storage = trace::core::MediaIoSource::classifyStorage(path);
        if (fileBacked && storage.remote) {
            lucidState_.status = trace::app::LucidIntegrationState::Status::Checking;
            startLucidProbe(path);
        }
    }

    shareState_ = trace::app::evaluateShare(path, fileBacked, lucidState_);
    syncShareActions();
}

// The probe runs on a pool thread that owns its own STA, because the handler
// registers ThreadingModel=Apartment and because loading a third-party shell
// extension is exactly the "blocking shell call" the spec keeps off the UI
// thread. Measured at 50-120ms on the nominated file, which is invisible here
// and would not have been on the tick.
void MainWindow::startLucidProbe(const QString& path) {
    const QPointer<MainWindow> guard(this);
    QThreadPool::globalInstance()->start([guard, path]() {
        const auto support = trace::app::probeLucidSupport(path);
        if (guard.isNull()) return;
        QMetaObject::invokeMethod(guard, [guard, path, support]() {
            if (guard.isNull()) return;
            // Discard a result for media that is no longer open. Without this a
            // slow probe on a LucidLink file could enable Copy Link for a local
            // file opened after it.
            if (guard->lucidProbePath_ != path) return;
            auto& st = guard->lucidState_;
            using Status = trace::app::LucidIntegrationState::Status;
            st.status = support.supported ? Status::Supported
                      : support.installed ? Status::Unsupported
                                          : Status::NotInstalled;
            st.reason = support.reason;
            guard->shareState_ = trace::app::evaluateShare(
                path, true, st);
            guard->syncShareActions();
            // The HUD is built on refresh, and a paused file does not refresh --
            // so without this the gate field keeps showing `lucid disabled` long
            // after the probe has said otherwise. That is a stale INSTRUMENT
            // rather than a stale gate, which is the more misleading of the two:
            // the menu was already correct while the HUD still accused it.
            guard->refreshHud("Lucid probe");
        }, Qt::QueuedConnection);
    });
}

void MainWindow::syncShareActions() {
    using trace::app::ShareAvailability;
    // Disabled and Unavailable render identically -- both are a greyed row with
    // a tooltip stating why -- and differ in what the tooltip SAYS. Trace does
    // not need two visual treatments to honour the distinction; it needs to not
    // hide either one, and to not tell the user "not right now" when the honest
    // answer is "not for this file, ever".
    const auto apply = [](QAction* action, ShareAvailability state, const QString& reason,
                          const QString& enabledTip) {
        if (!action) return;
        const bool on = state == ShareAvailability::Available;
        action->setEnabled(on);
        action->setToolTip(on ? enabledTip : reason);
        // The accessibility name the spec requires to stay synchronized with the
        // rest of the action. It costs one line here and it is the only
        // accessibility affordance the Share commands have on the overlay path.
        action->setStatusTip(on ? enabledTip : reason);
    };

    apply(copyFilePathAction_, shareState_.copyPath, shareState_.copyPathReason,
          tr("Copy the file's full Windows path to the clipboard"));
    apply(showInExplorerAction_, shareState_.showInExplorer, shareState_.showInExplorerReason,
          tr("Open the containing folder with this file selected"));
    apply(copyLucidLinkAction_, shareState_.lucidLink, shareState_.lucidLinkReason,
          tr("Copy a direct LucidLink link to this file"));
}

void MainWindow::showShareMenu(const QPoint& globalPos) {
    if (!shareMenu_) return;
    // The gate is re-evaluated as the menu opens, not only at open time. A file
    // can be deleted or a mount can drop while it is on screen, and this is the
    // one moment the user is about to act on the answer -- it costs one cached
    // volume lookup and it is not on any per-frame path.
    refreshShareState();
    // popup(), not exec(). exec() runs a nested event loop, and on the D3D11
    // backend this is reached from inside a raw WM_LBUTTONUP handler on the
    // child surface window -- blocking there would run the whole menu's
    // lifetime inside a window procedure.
    shareMenu_->popup(globalPos);
}

// Spec phase 10. The one place the view transform changes.
//
// Every action routes through here, so "what is on screen" and "what the menu
// says" come from a single write followed by a single read-back -- the shape
// phase 2 gave fullscreen, and for the same reason: five handlers each setting
// their own state is five chances for the menu and the picture to disagree.
//
// It asks the VIEWER for the resulting transform rather than assuming `next`
// took, which is what makes the tick marks honest if the viewer ever declines a
// transform.
void MainWindow::applyViewTransform(const trace::render::ViewTransform& next,
                                    const char* action) {
    if (!viewer_) return;
    viewer_->setViewTransform(next);
    syncViewTransformActions();
    // Section 4: "re-evaluate the ratio after a temporary 90-degree rotation"
    // and "restore the original media ratio when transforms reset". Both are the
    // same call, because the ratio is composed from media shape and session
    // transform every time rather than remembered -- so Reset needs no separate
    // path and cannot drift from Rotate.
    applyMediaWindowShape();
    // No decoder request and no reload: the frame on screen is the frame that
    // was already there, drawn through a different coordinate transform. That is
    // the whole reason this is cheap during playback, and it is why the HUD's
    // frame index and the source timecode cannot move here.
    //
    // repaint(), NOT update(), and the difference is visible in the HUD. The fit
    // and the reduction taps are measured BY the paint and reported afterwards,
    // so refreshing the HUD after a merely-scheduled repaint prints the previous
    // transform's `display` -- and on a paused file nothing refreshes it again,
    // so it stays wrong. Measured before the fix: a 4x5 clip rotated 90 degrees
    // drew landscape while `display` still read `288x360`. This is the same
    // reason the scrub walk calls repaint().
    viewer_->repaint();
    refreshHud(action);
    // Spec phase 13. The inspector's "Orientation on screen" and "Current scale"
    // are both observed rows and both move here. Through the timer like every
    // other route, even though the repaint above has already happened -- one
    // refresh path is what stops a later caller getting the ordering wrong.
    scheduleInspectorRefresh();
}

void MainWindow::syncViewTransformActions() {
    if (!viewer_) return;
    const auto vt = viewer_->viewTransform();
    if (flipHorizontalAction_) {
        QSignalBlocker block(flipHorizontalAction_);
        flipHorizontalAction_->setChecked(vt.flipH);
    }
    if (flipVerticalAction_) {
        QSignalBlocker block(flipVerticalAction_);
        flipVerticalAction_->setChecked(vt.flipV);
    }
    // Disabled at identity: there is nothing to reset, and a Reset that is
    // always enabled says nothing about whether a transform is in force. This is
    // the design package's Disabled state -- the action exists and cannot run
    // right now -- rather than Unavailable, which is about the media.
    if (resetViewTransformAction_) resetViewTransformAction_->setEnabled(!vt.isIdentity());
}

// Spec phase 15. Two ticks and four enables, all read off the viewer.
//
// FIT AND ACTUAL SIZE ARE TICKED, ZOOM IN AND ZOOM OUT ARE NOT, and that
// asymmetry is the honest one: fit and 1:1 are named states a picture can BE
// in, while the zooms are steps. A checkable "Zoom In" would have to mean
// something, and there is nothing for it to mean.
void MainWindow::syncViewScaleActions() {
    if (!viewer_) return;
    const bool haveMedia = currentMedia_.has_value();
    const bool fit = viewer_->isFitToWindow();
    // Within a thousandth of 1:1 rather than exactly, because the scale can be
    // reached by the ladder (exactly 1.0) or by the cap (a computed double).
    const bool actual = !fit && std::abs(viewer_->currentScale() - 1.0) < 0.001;
    if (actualSizeAction_) {
        QSignalBlocker block(actualSizeAction_);
        actualSizeAction_->setChecked(actual);
        actualSizeAction_->setEnabled(haveMedia);
    }
    if (fitToWindowAction_) {
        QSignalBlocker block(fitToWindowAction_);
        fitToWindowAction_->setChecked(fit);
        // ENABLED WHILE ACTIVE, SHOWING ITS CHECKED STATE (owner, 2026-08-11).
        // The first cut disabled it while already fitting, reasoning that a
        // command which visibly does nothing is the showInfo failure. That
        // reasoning does not apply to a CHECKABLE item: the tick is what it
        // says, and greying the row makes the current state read as
        // unavailable. Actual Size is enabled while checked for the same
        // reason, so the two rows now behave alike -- which they did not.
        fitToWindowAction_->setEnabled(haveMedia);
    }
    if (zoomInAction_) zoomInAction_->setEnabled(haveMedia);
    if (zoomOutAction_) zoomOutAction_->setEnabled(haveMedia);
}

void MainWindow::copyMediaFilePath() {
    refreshShareState();
    QString error;
    if (!trace::app::copyPathToClipboard(shareState_, error)) {
        statusBar()->showMessage(error, 3000);
        return;
    }
    // The spec's "non-blocking confirmation". The status bar is what every other
    // confirmation in Trace already uses, so this needed no new mechanism.
    statusBar()->showMessage(tr("File path copied."), 2000);
}

void MainWindow::showMediaInFileExplorer() {
    refreshShareState();
    trace::app::revealInFileExplorer(shareState_, this, [this](const QString& message) {
        statusBar()->showMessage(message, 3000);
    });
}

// Spec phase 9. The integration produces the link; Trace runs its command,
// validates the form of what came back, and reports. It never composes one.
void MainWindow::copyLucidLinkForMedia() {
    refreshShareState();
    if (qgetenv("TRACE_LUCID_LOG") == "1") {
        fprintf(stderr, "[lucid] action triggered; gate=%s\n",
                trace::app::shareAvailabilityName(shareState_.lucidLink));
        fflush(stderr);
    }
    if (shareState_.lucidLink != trace::app::ShareAvailability::Available) {
        QString why = shareState_.lucidLinkReason;
        if (why.isEmpty()) why = tr("A LucidLink link is not available for this file.");
        statusBar()->showMessage(why, 4000);
        return;
    }
    // One at a time. The command reaches the LucidLink daemon and can take a
    // moment, and a second press would race two clipboard observers against each
    // other -- both watching the same sequence number, either able to attribute
    // the other's result to itself.
    if (lucidCopyInFlight_) {
        statusBar()->showMessage(tr("Still asking LucidLink for the link..."), 2000);
        return;
    }
    lucidCopyInFlight_ = true;
    statusBar()->showMessage(tr("Asking LucidLink for the link..."), 4000);

    const QString path = shareState_.canonicalPath;
    const QPointer<MainWindow> guard(this);
    QThreadPool::globalInstance()->start([guard, path]() {
        const auto result = trace::app::copyLucidLinkViaShell(path);
        if (guard.isNull()) return;
        QMetaObject::invokeMethod(guard, [guard, result]() {
            if (guard.isNull()) return;
            guard->lucidCopyInFlight_ = false;
            if (result.ok) {
                guard->statusBar()->showMessage(tr("LucidLink link copied."), 2000);
            } else {
                // Failure is reported and the clipboard is whatever
                // copyLucidLinkViaShell left it as -- which is either untouched
                // or restored. Trace never writes a guessed value.
                guard->statusBar()->showMessage(result.error, 5000);
            }
        }, Qt::QueuedConnection);
    });
}

QString MainWindow::sourceTimecodeAt(long long frame) const {
    const auto tc = trace::core::TimeFormat::framesToTimecode(
        sourceTimecodeStartFrames_ + std::max(0LL, frame),
        timecodeFpsNum_, timecodeFpsDen_, sourceTimecodeDropFrame_);
    return trace::core::TimeFormat::formatTimecode(tc);
}

long long MainWindow::frameForSourceTimecode(const QString& text) const {
    if (!hasSourceTimecode_ || !frameSource_) return -1;
    trace::core::TimeFormat::Timecode parsed;
    if (!trace::core::TimeFormat::parseTimecode(text, parsed)) return -1;
    // The typed value is read in the SOURCE's convention, not in whatever
    // separator the user happened to type: a file is drop-frame or it is not,
    // and letting a semicolon change the arithmetic would make the same string
    // mean two different frames.
    parsed.dropFrame = sourceTimecodeDropFrame_;
    if (parsed.frames >= trace::core::TimeFormat::nominalRate(timecodeFpsNum_, timecodeFpsDen_)) return -1;

    const long long abs =
        trace::core::TimeFormat::timecodeToFrames(parsed, timecodeFpsNum_, timecodeFpsDen_);
    const long long index = abs - sourceTimecodeStartFrames_;
    if (index < 0 || index > frameSource_->maxFrame()) return -1;
    return index;
}

// THE FIRST TEXT-ENTRY CONTROLS IN TRACE, and that is the interesting part of
// them rather than the dialogs themselves.
//
// Every single-key command in ShortcutTable is dispatched from
// MainWindow::keyPressEvent, which matches on the key and ignores modifiers --
// so H, J, K, L, E, F, S, T and M are all candidates to be eaten while someone
// is typing. Qt's mechanism is what stops it: QLineEdit accepts
// QEvent::ShortcutOverride for printable keys, which suppresses the shortcut and
// delivers the keystroke to the widget, and a modal QInputDialog is a separate
// window whose key events never reach this one at all. Both were untestable
// until this commit because there was nothing to type into; they are tested now
// (see the phase 7 record).
//
// QInputDialog rather than a hand-built dialog: it is modal, it owns its own
// Escape and Return, and it is what QApplication::activeModalWidget() reports --
// which is how the overlay's holdVisible hook knows not to fade the transport
// out from under an open prompt. A hand-rolled non-modal panel would have had to
// reimplement all four of those.
void MainWindow::promptGoToFrame() {
    if (!frameSource_) return;
    const long long maxFrame = frameSource_->maxFrame();
    if (maxFrame < 0) return;

    bool ok = false;
    // Zero-based, and the range is stated in the prompt rather than left to be
    // discovered by being rejected. `maxFrame` is the last valid INDEX, which is
    // what the transport bar has always printed.
    const int value = QInputDialog::getInt(
        this, tr("Go to Frame"),
        tr("Frame (0 - %1):").arg(maxFrame),
        static_cast<int>(playback_.state().currentFrame),
        0, static_cast<int>(std::min<long long>(maxFrame, std::numeric_limits<int>::max())),
        1, &ok);
    if (!ok) return;
    goToFrame(value, "Go to Frame");
}

void MainWindow::promptGoToTimecode() {
    // Guarded here as well as by the action's enabled state. The action can only
    // be disabled if something remembered to disable it; this cannot be reached
    // wrongly at all.
    if (!hasSourceTimecode_ || !frameSource_) return;

    bool ok = false;
    const QString text = QInputDialog::getText(
        this, tr("Go to Timecode"),
        tr("Timecode (%1 - %2):")
            .arg(sourceTimecodeAt(0))
            .arg(sourceTimecodeAt(std::max(0LL, frameSource_->maxFrame()))),
        QLineEdit::Normal, sourceTimecodeAt(playback_.state().currentFrame), &ok);
    if (!ok) return;

    const long long frame = frameForSourceTimecode(text);
    if (frame < 0) {
        // Validated BEFORE seeking, and refused rather than clamped. Clamping a
        // mistyped timecode would move the playhead somewhere the user did not
        // ask for and look like it had worked.
        statusBar()->showMessage(
            tr("Not a timecode in this media: %1").arg(text.trimmed()), 4000);
        refreshHud("Go to Timecode: rejected");
        return;
    }
    goToFrame(frame, "Go to Timecode");
}

// ---- Movie Inspector (spec phase 13) ---------------------------------------

namespace {

QString formatByteSize(qint64 bytes) {
    if (bytes < 0) return MainWindow::tr("Unknown");
    // Exact byte count alongside the readable one. A review tool gets asked
    // "is this the same file as the one on the server", and the rounded figure
    // cannot answer it.
    const QString exact = QLocale::system().toString(bytes) + MainWindow::tr(" bytes");
    static const char* units[] = {"KB", "MB", "GB", "TB"};
    double value = static_cast<double>(bytes);
    int unit = -1;
    while (value >= 1024.0 && unit < 3) {
        value /= 1024.0;
        ++unit;
    }
    if (unit < 0) return exact;
    return QStringLiteral("%1 %2 (%3)")
        .arg(QString::number(value, 'f', value < 10.0 ? 2 : 1))
        .arg(QLatin1String(units[unit]))
        .arg(exact);
}

QString formatBitrate(double bitsPerSecond) {
    if (bitsPerSecond <= 0.0) return {};
    if (bitsPerSecond >= 1'000'000.0)
        return QStringLiteral("%1 Mbps").arg(QString::number(bitsPerSecond / 1'000'000.0, 'f', 2));
    return QStringLiteral("%1 kbps").arg(QString::number(bitsPerSecond / 1000.0, 'f', 1));
}

// The rotation and flips the user is actually looking at, as a sentence.
QString describeOrientation(int quarterTurns, bool flipH, bool flipV) {
    QStringList parts;
    if (quarterTurns) {
        parts << MainWindow::tr("rotated %1° clockwise").arg(quarterTurns * 90);
    }
    if (flipH) parts << MainWindow::tr("flipped horizontally");
    if (flipV) parts << MainWindow::tr("flipped vertically");
    if (parts.isEmpty()) return MainWindow::tr("Upright, unflipped");
    QString text = parts.join(QStringLiteral(", "));
    text[0] = text[0].toUpper();
    return text;
}

} // namespace

// EVERYTHING THE INSPECTOR SHOWS, AND NOTHING IT HAD TO ASK FOR.
//
// Two of the spec's rules govern this function rather than the window: "do not
// continuously poll expensive decoder state" and "do not block remote-media
// opening to calculate optional values". Every value below is either read once
// at open (the colour tags, the codec, the audio stream, the file size) or
// already maintained for something else (the drawn size, the view transform),
// so building a snapshot costs string formatting and no I/O at all.
//
// The decoder's METADATA is safe to read live -- only open() writes it, and
// open() cannot run while a scrub lease is out. Its PERF STATS are not, which
// is why the file size and the playback colour state come from `hudPerf_`, the
// snapshot captureDecoderTelemetry() maintains from whichever side owns the
// decoder. Reading videoDecoder_.perfStats() here would be the one thing the
// lease exists to prevent.
trace::app::InspectorSnapshot MainWindow::buildInspectorSnapshot() const {
    using trace::app::FieldOrigin;
    using trace::app::InspectorField;
    using trace::app::InspectorSection;

    trace::app::InspectorSnapshot snap;
    if (!currentMedia_.has_value() || !frameSource_) return snap;

    const QString path = QString::fromStdString(currentMedia_->path);
    // The canonical path the Share gate computed at open, not a fresh
    // QFileInfo: two answers to "what is this file's path" eventually disagree,
    // which is the reason MediaShare::canonicalNativePath was exported at phase
    // 11 rather than reimplemented.
    snap.sourcePath = shareState_.canonicalPath.isEmpty() ? path : shareState_.canonicalPath;
    // The basename by searching the string, RecentFiles' rule and for its
    // reason: QFileInfo is the call that must not appear on this path.
    const int slash = std::max(snap.sourcePath.lastIndexOf(QLatin1Char('/')),
                               snap.sourcePath.lastIndexOf(QLatin1Char('\\')));
    snap.fileName = slash >= 0 ? snap.sourcePath.mid(slash + 1) : snap.sourcePath;

    const bool isVideo = currentMedia_->kind == MediaKind::VideoFile;
    const auto& vm = videoDecoder_.metadata();
    const auto& perf = hudPerf_;
    const auto& drawPerf = viewer_ ? viewer_->perfStats() : trace::ui::ViewerPerfStats{};
    const trace::render::ViewTransform view =
        viewer_ ? viewer_->viewTransform() : trace::render::ViewTransform{};

    // ---- observed geometry, computed once and used by two rows -------------
    //
    // lastDrawSize is DEVICE pixels and is measured BY the paint (phase 10), so
    // it is only current because refreshInspector() is reached from a timer that
    // fires after one. See scheduleInspectorRefresh().
    const QSize drawn = drawPerf.lastDrawSize;
    QSize naturalOnScreen;
    if (isVideo) naturalOnScreen = view.apply(vm.naturalDisplaySize());
    // width/height, NOT image.size(): LoadedImageInfo::image is left default at
    // both sites that build one, so its size is empty. See currentDisplayAspect.
    else if (currentImage_.has_value())
        naturalOnScreen = view.apply(QSize(currentImage_->width, currentImage_->height));

    // ---- General -----------------------------------------------------------
    InspectorSection general;
    general.title = tr("General");
    general.fields.push_back({tr("File name"), snap.fileName, FieldOrigin::File});
    // `entry`: a path is one unbroken token and gets a read-only field rather
    // than a wrapping label. See InspectorField::entry for the two layout faults
    // that produced the rule.
    general.fields.push_back({tr("Source path"), snap.sourcePath, FieldOrigin::File, true});

    if (isVideo) {
        general.fields.push_back(
            {tr("Resolution"), tr("%1 × %2").arg(vm.width).arg(vm.height), FieldOrigin::Encoded});
    } else if (currentImage_.has_value()) {
        general.fields.push_back({tr("Resolution"),
                                  tr("%1 × %2").arg(currentImage_->width).arg(currentImage_->height),
                                  FieldOrigin::Encoded});
    }

    // THE FILE SIZE COMES FROM THE OPEN, NEVER FROM A STAT ISSUED HERE.
    // MediaIoSource read it while opening the file; a still or a sequence frame
    // does not go through the decoder, so openPath takes it there instead --
    // also at open, also on a path being touched anyway.
    const qint64 bytes = isVideo ? perf.sourceBytes : openedFileBytes_;
    general.fields.push_back({tr("File size"), formatByteSize(bytes), FieldOrigin::File});

    if (isVideo) {
        const QString rate = formatBitrate(perf.sourceBitrateMbps * 1'000'000.0);
        general.fields.push_back(
            {tr("Overall data rate"), rate.isEmpty() ? tr("Unknown") : rate, FieldOrigin::File});

        // DURATION, added at spec phase 14 and carried from phase 13's sign-off.
        //
        // The owner's field list named it, the window had never had it, and the
        // spec's own field list for the inspector does not ask for one -- so it
        // was recorded as a discrepancy rather than quietly inserted into a
        // closed phase, and taken as a decision here.
        //
        // `encoded`, not `observed`: this is the container's claim about the
        // media's length. It is NOT the frame count divided by the frame rate,
        // and the two genuinely disagree on files whose last packet is short --
        // which is exactly the kind of difference an inspector exists to show
        // rather than to smooth over. The frame count is printed beside it for
        // that reason.
        if (vm.durationSeconds > 0.0) {
            const double totalSeconds = vm.durationSeconds;
            const int hours = static_cast<int>(totalSeconds) / 3600;
            const int minutes = (static_cast<int>(totalSeconds) % 3600) / 60;
            const double seconds = totalSeconds - hours * 3600.0 - minutes * 60.0;
            QString duration =
                hours > 0
                    ? tr("%1:%2:%3")
                          .arg(hours)
                          .arg(minutes, 2, 10, QLatin1Char('0'))
                          .arg(seconds, 6, 'f', 3, QLatin1Char('0'))
                    : tr("%1:%2").arg(minutes).arg(seconds, 6, 'f', 3, QLatin1Char('0'));
            if (vm.frameCount > 0) {
                duration += tr(" (%1 frames)").arg(vm.frameCount);
            }
            general.fields.push_back({tr("Duration"), duration, FieldOrigin::Encoded});
        } else {
            general.fields.push_back({tr("Duration"), tr("Unknown"), FieldOrigin::Encoded});
        }
    }

    // Current viewport size. Reported as the VIDEO AREA, and the viewer's own
    // size is named alongside it whenever the two differ -- with the aspect lock
    // on they are the same rectangle by construction (spec section 4 eliminates
    // the bars), and with it off they are not.
    QString viewport = tr("Unknown");
    if (viewer_ && drawn.isValid() && !drawn.isEmpty()) {
        const double dpr = viewer_->devicePixelRatioF();
        const QSize viewerDevice(static_cast<int>(std::lround(viewer_->width() * dpr)),
                                 static_cast<int>(std::lround(viewer_->height() * dpr)));
        viewport = tr("%1 × %2 px (video area)").arg(drawn.width()).arg(drawn.height());
        if (viewerDevice != drawn) {
            viewport += tr("; viewer is %1 × %2 px").arg(viewerDevice.width()).arg(viewerDevice.height());
        }
        if (dpr != 1.0) {
            viewport += tr(" — device pixels at %1% scaling")
                            .arg(QString::number(dpr * 100.0, 'f', 0));
        }
    }
    general.fields.push_back({tr("Current viewport size"), viewport, FieldOrigin::Observed});

    if (isVideo) {
        QString container = vm.containerLongName.isEmpty() ? vm.containerName : vm.containerLongName;
        if (container.isEmpty()) container = tr("Unknown");
        else if (!vm.containerName.isEmpty() && !vm.containerLongName.isEmpty())
            container = tr("%1 (%2)").arg(vm.containerLongName, vm.containerName);
        general.fields.push_back({tr("Container"), container, FieldOrigin::Encoded});

        QString videoFormat = vm.codecName;
        if (!vm.codecProfile.isEmpty()) videoFormat += tr(" (%1)").arg(vm.codecProfile);
        videoFormat += tr(", %1 fps").arg(QString::number(vm.fps, 'f', 3));
        general.fields.push_back({tr("Video format"), videoFormat, FieldOrigin::Encoded});

        QString audioFormat = tr("None");
        if (vm.hasAudio) {
            audioFormat = vm.audioCodecName;
            if (!vm.audioProfile.isEmpty()) audioFormat += tr(" (%1)").arg(vm.audioProfile);
            audioFormat += tr(", %1 Hz").arg(vm.audioSampleRate);
            if (!vm.audioChannelLayout.isEmpty()) audioFormat += tr(", %1").arg(vm.audioChannelLayout);
        }
        general.fields.push_back({tr("Audio format"), audioFormat, FieldOrigin::Encoded});
    } else {
        general.fields.push_back({tr("Video format"),
                                  currentMedia_->kind == MediaKind::ImageSequence
                                      ? tr("Image sequence")
                                      : tr("Still image"),
                                  FieldOrigin::Encoded});
    }
    snap.sections.push_back(std::move(general));

    // ---- Video details -----------------------------------------------------
    InspectorSection video;
    video.title = isVideo ? tr("Video details") : tr("Image details");

    if (isVideo) {
        video.fields.push_back({tr("Frame rate"),
                                tr("%1/%2 (%3 fps)")
                                    .arg(vm.fpsNum)
                                    .arg(vm.fpsDen)
                                    .arg(QString::number(vm.fps, 'f', 6)),
                                FieldOrigin::Encoded});

        // Start timecode is not in the spec's field list, and it is here because
        // phase 7 read it from the container under exactly this phase's rule --
        // absent stays absent, never synthesised from zero. A metadata panel that
        // omitted it would be the one place in Trace that does not report it.
        video.fields.push_back(
            {tr("Start timecode"),
             vm.hasStartTimecode
                 ? tr("%1 (%2)").arg(vm.startTimecode,
                                     vm.startTimecodeDropFrame ? tr("drop frame") : tr("non-drop"))
                 : tr("None — the container states no timecode"),
             FieldOrigin::Encoded});

        const QString streamRate = formatBitrate(static_cast<double>(vm.videoBitrateBps));
        video.fields.push_back({tr("Video bitrate"),
                                streamRate.isEmpty() ? tr("Not stated by the container")
                                                     : streamRate,
                                FieldOrigin::Encoded});

        // `sarStated` is phase 12's negative control, and it is exactly the
        // distinction this window exists to make: "1:1 because the file says so"
        // and "1:1 because nobody said" are different claims. Three of the four
        // shipping assets state 1:1; the 9:16 clip states nothing.
        video.fields.push_back({tr("Pixel aspect ratio"),
                                tr("%1:%2 %3")
                                    .arg(vm.sarNum)
                                    .arg(vm.sarDen)
                                    .arg(vm.sarStated ? tr("(stated)")
                                                      : tr("(assumed square — the container states none)")),
                                FieldOrigin::Encoded});

        QString dar = QString::number(vm.displayAspect(), 'f', 4);
        {
            long long an = static_cast<long long>(vm.width) * vm.sarNum;
            long long ad = static_cast<long long>(vm.height) * vm.sarDen;
            if (vm.rotationDegrees == 90 || vm.rotationDegrees == 270) std::swap(an, ad);
            const long long g = std::gcd(an, ad);
            if (g > 0) {
                an /= g;
                ad /= g;
                // Only when it reduces to something a person can read. An exact
                // 3839:2160 is true and useless, and printing it would make the
                // useful cases harder to spot.
                if (an <= 999 && ad <= 999) dar = tr("%1:%2 (%3)").arg(an).arg(ad).arg(dar);
            }
        }
        video.fields.push_back({tr("Display aspect ratio"), dar, FieldOrigin::Encoded});

        video.fields.push_back(
            {tr("Container rotation"),
             vm.rotationDegrees == 0 && !vm.rotationSnapped
                 ? tr("None")
                 : tr("%1° clockwise%2")
                       .arg(vm.rotationDegrees)
                       .arg(vm.rotationSnapped
                                ? tr(" (snapped — the display matrix is not a quarter turn)")
                                : QString()),
             FieldOrigin::Encoded});
    }

    // Current scale, against the size the media is MEANT to be shown at, with
    // the view transform applied so a rotated picture is not reported as scaled
    // by the ratio of two different axes.
    QString scale = tr("Unknown");
    if (naturalOnScreen.width() > 0 && drawn.width() > 0) {
        const double factor = static_cast<double>(drawn.width()) /
                              static_cast<double>(naturalOnScreen.width());
        scale = drawPerf.lastDrawWasScaled
                    ? tr("%1% of natural displayed size (%2 × %3)")
                          .arg(QString::number(factor * 100.0, 'f', 1))
                          .arg(naturalOnScreen.width())
                          .arg(naturalOnScreen.height())
                    : tr("1:1 — drawn at natural displayed size");
    }
    video.fields.push_back({tr("Current scale"), scale, FieldOrigin::Observed});

    // ORIENTATION ON SCREEN, WHICH IS THE COMPOSITION AND NOT EITHER HALF.
    // ViewerWidget::applySourceShape is the one place the container's rotation
    // and the user's are combined; this reports the same sum, and names both
    // contributions so "the file is sideways" and "I rotated it" stay distinct.
    {
        const int containerTurns = viewer_ ? viewer_->sourceRotationDegrees() / 90 : 0;
        const int composed = ((view.quarterTurns + containerTurns) % 4 + 4) % 4;
        video.fields.push_back(
            {tr("Orientation on screen"),
             tr("%1 (file %2°, view transform %3°)")
                 .arg(describeOrientation(composed, view.flipH, view.flipV))
                 .arg(containerTurns * 90)
                 .arg(view.quarterTurns * 90),
             FieldOrigin::Observed});
    }

    if (isVideo) {
        // The ENCODED format, from the metadata read at open -- not
        // VideoPerfStats::srcPixelFormat, which is what the last conversion saw
        // and carries " (a-skip)" once playback drops an alpha plane.
        video.fields.push_back({tr("Pixel format"),
                                vm.pixelFormatName.isEmpty() ? tr("Unknown") : vm.pixelFormatName,
                                FieldOrigin::Encoded});
        video.fields.push_back(
            {tr("Bit depth"),
             vm.bitsPerComponent > 0
                 ? tr("%1-bit per component (%2 bits per pixel)")
                       .arg(vm.bitsPerComponent)
                       .arg(vm.bitsPerPixel)
                 : tr("Unknown"),
             FieldOrigin::Encoded});

        // THE FOUR TAGS, VERBATIM INCLUDING THEIR ABSENCE. This is the rule the
        // whole phase was shaped by: two of the four shipping assets state
        // nothing at all here, and they are precisely the two whose HUD reads
        // `bt709*`, so an inspector built on VideoPerfStats would tell the user
        // they are tagged BT.709. In a review tool that is a bug report about
        // the media rather than about Trace.
        const QString untagged = tr("Untagged");
        video.fields.push_back({tr("Colour primaries"),
                                vm.taggedColorPrimaries.isEmpty() ? untagged : vm.taggedColorPrimaries,
                                FieldOrigin::Encoded});
        video.fields.push_back({tr("Transfer characteristics"),
                                vm.taggedColorTransfer.isEmpty() ? untagged : vm.taggedColorTransfer,
                                FieldOrigin::Encoded});
        video.fields.push_back({tr("Matrix coefficients"),
                                vm.taggedColorMatrix.isEmpty() ? untagged : vm.taggedColorMatrix,
                                FieldOrigin::Encoded});
        video.fields.push_back({tr("Range"),
                                !vm.hasTaggedRange
                                    ? untagged
                                    : (vm.taggedRangeIsFull ? tr("Full") : tr("Limited")),
                                FieldOrigin::Encoded});

        // AND WHAT TRACE DID ABOUT IT, on its own row and under its own origin.
        // swsCoefficientsFor applies the standard "HD and up is 709" heuristic
        // to an untagged file: correct for decoding, and an answer Trace
        // invented. Showing both is what "distinguish encoded metadata from
        // playback inference" asks for -- the alternative is showing one of them
        // and hoping the user knows which.
        QString playback = tr("%1 matrix (%2), %3 range")
                               .arg(perf.colorMatrix.isEmpty() ? tr("unknown") : perf.colorMatrix)
                               .arg(perf.colorMatrixInferred
                                        ? tr("inferred by Trace — the file states none")
                                        : tr("as tagged"))
                               .arg(perf.srcFullRange ? tr("full") : tr("limited"));
        if (!perf.srcPixelFormat.isEmpty() && !perf.dstPixelFormat.isEmpty()) {
            playback += tr(" · converting %1 → %2").arg(perf.srcPixelFormat, perf.dstPixelFormat);
        }
        video.fields.push_back({tr("Playback is using"), playback, FieldOrigin::Playback});

        QString codec = vm.codecName;
        if (!vm.codecProfile.isEmpty()) codec += tr(" · %1").arg(vm.codecProfile);
        video.fields.push_back({tr("Codec / profile"), codec, FieldOrigin::Encoded});
        // The CONTAINER's track id, which is what an editorial tool means by
        // "track ID". FFmpeg's array position is named separately rather than
        // substituted for it.
        video.fields.push_back({tr("Track ID"),
                                tr("%1 (stream index %2)").arg(vm.videoTrackId).arg(vm.videoStreamIndex),
                                FieldOrigin::Encoded});
    }
    snap.sections.push_back(std::move(video));

    // ---- Audio details -----------------------------------------------------
    if (isVideo) {
        InspectorSection audioSection;
        audioSection.title = tr("Audio details");
        if (!vm.hasAudio) {
            audioSection.fields.push_back(
                {tr("Audio"), tr("No audio track"), FieldOrigin::Encoded});
        } else {
            QString codec = vm.audioCodecName;
            if (!vm.audioProfile.isEmpty()) codec += tr(" · %1").arg(vm.audioProfile);
            audioSection.fields.push_back({tr("Codec"), codec, FieldOrigin::Encoded});
            audioSection.fields.push_back(
                {tr("Sample rate"), tr("%1 Hz").arg(vm.audioSampleRate), FieldOrigin::Encoded});
            const QString aRate = formatBitrate(static_cast<double>(vm.audioBitrateBps));
            audioSection.fields.push_back({tr("Bitrate"),
                                           aRate.isEmpty() ? tr("Not stated by the container") : aRate,
                                           FieldOrigin::Encoded});
            audioSection.fields.push_back(
                {tr("Channel layout"),
                 tr("%1 (%2 channels)")
                     .arg(vm.audioChannelLayout.isEmpty() ? tr("unknown") : vm.audioChannelLayout)
                     .arg(vm.audioChannels),
                 FieldOrigin::Encoded});
            audioSection.fields.push_back(
                {tr("Track ID"),
                 tr("%1 (stream index %2)").arg(vm.audioTrackId).arg(vm.audioStreamIndex),
                 FieldOrigin::Encoded});
        }
        snap.sections.push_back(std::move(audioSection));
    }

    return snap;
}

void MainWindow::refreshInspector() {
    if (!inspector_ || !inspector_->isVisible()) return;
    inspector_->setSnapshot(buildInspectorSnapshot());
}

// ARMED BY EVENTS, NOT RUNNING ON A SCHEDULE, AND SILENT WHEN THE WINDOW IS
// SHUT. That combination is what keeps "do not continuously poll" true while
// still satisfying "update when active media changes".
//
// The deferral is phase 10's trap: `RenderStats::lastDrawSize` and the fit it
// implies are measured BY the paint, so a refresh issued at the moment the
// window changed reports the PREVIOUS viewport, and on a paused file nothing
// ever comes along to correct it. Waiting for the paint is the whole reason
// this is a timer rather than a direct call -- and it also collapses the ~123
// resize events of a real drag into one rebuild, which is what keeps the
// instrument out of the path the phase 12 record ruled it out of for the HUD.
void MainWindow::scheduleInspectorRefresh() {
    if (!inspector_ || !inspector_->isVisible()) return;
    inspectorRefreshTimer_.start();
}

void MainWindow::toggleMovieInspector(bool show) {
    if (!inspector_) {
        if (!show) return;
        // Created on first use, so a session that never opens it pays nothing.
        inspector_ = new trace::app::MovieInspector(this);
        // The menu item FOLLOWS the window rather than assuming it drove every
        // change: the title bar's close button is a route the action never sees,
        // and a checkable item that says "shown" over a closed window is the
        // kind of small disagreement this pass exists to remove.
        connect(inspector_, &trace::app::MovieInspector::visibilityChanged, this,
                [this](bool visible) {
                    if (inspectorAction_) inspectorAction_->setChecked(visible);
                    // Hidden: stop any refresh already armed, so nothing runs for
                    // a window nobody is looking at.
                    if (!visible) inspectorRefreshTimer_.stop();
                });
    }
    if (show) {
        // Filled synchronously here, and this is the one place a direct read is
        // the RIGHT answer rather than the trap: nothing has just changed, so
        // the last paint's drawn size is the size on screen. Everywhere else
        // goes through the timer.
        inspector_->setSnapshot(buildInspectorSnapshot());
        inspector_->show();
        inspector_->raise();
        inspector_->activateWindow();
    } else {
        inspector_->hide();
    }
}

// ============================================================================
// Spec phase 14 -- menus, help and window commands.
// ============================================================================

// What build this is, in one place, because About and the Report an Issue mail
// body both have to say it and two answers would be worse than none.
//
// The renderer is read from the ADOPTED one, never from TRACE_RENDERER: a GPU
// backend that failed to initialize has already been replaced by the CPU one,
// and an issue report naming the requested backend rather than the running one
// would send every fallback investigation down the wrong path.
QString MainWindow::buildIdentity() const {
    QString renderer = tr("unknown");
    if (viewer_) renderer = viewer_->rendererName();
    return tr("Trace %1 (alpha)\nQt %2\nRenderer: %3\nTransport: %4")
        .arg(QStringLiteral(TRACE_VERSION_STRING))
        .arg(QString::fromLatin1(qVersion()))
        .arg(renderer)
        .arg(trace::render::OverlayModel::enabledByEnvironment() ? tr("overlay")
                                                                 : tr("docked bar"));
}

void MainWindow::showAboutDialog() {
    QMessageBox::about(
        this, tr("About Trace"),
        tr("<h3>Trace %1</h3>"
           "<p>A fast, minimal media player for review work.</p>"
           "<p style='white-space:pre'>Qt %2<br>Renderer: %3</p>"
           "<p><small>Alpha. Frame order, stepping and timing are exact; "
           "formats and interface are still being filled in.</small></p>")
            .arg(QStringLiteral(TRACE_VERSION_STRING))
            .arg(QString::fromLatin1(qVersion()))
            .arg(viewer_ ? viewer_->rendererName() : tr("unknown")));
}

// REAL CONTENT, and the four things it says are the four that are not guessable
// from the interface: that the arrows are exact rather than approximate, that
// the side buttons enter the ladder at 2x while J and L enter at 1x, that
// scrubbing never skips a frame on release however it looked during the drag,
// and that the diagnostics HUD exists at all.
//
// It is deliberately short. A help window nobody reads is the same failure as a
// menu item that does nothing, and the full keyboard contract is one menu item
// away in a window that is generated rather than written.
void MainWindow::showTraceHelp() {
    QMessageBox box(this);
    box.setWindowTitle(tr("Trace Help"));
    box.setTextFormat(Qt::RichText);
    box.setText(tr("<h3>Trace</h3>"
                   "<p>Open a render, check a frame, move on. Trace is a viewer, "
                   "not an editor or an asset manager.</p>"));
    box.setInformativeText(
        tr("<p><b>Stepping is exact.</b> Left and Right move exactly one frame. "
           "\"Next frame\" always means the actual next frame, including "
           "backwards.</p>"
           "<p><b>Rewind and Fast-forward start at 2x</b> and climb "
           "2x - 5x - 10x - 30x on each press. The <b>J</b> and <b>L</b> keys do "
           "the same thing but start at 1x, so <b>L</b> is ordinary playback and "
           "<b>K</b> stops. Sound plays at 1x forward only.</p>"
           "<p><b>Scrubbing may look soft while you drag</b> on heavy media, and "
           "always lands on the exact frame you release on, at full "
           "resolution.</p>"
           "<p><b>Press H</b> for the diagnostics overlay - frame timing, cache "
           "and decode figures. <b>Ctrl+I</b> opens the Movie Inspector, which "
           "says where every value came from.</p>"
           "<p>The complete key list is in <b>Help &gt; Keyboard Shortcuts</b>.</p>"));
    box.setIcon(QMessageBox::NoIcon);
    box.exec();
}

// The Keyboard Shortcuts window, rendered from ShortcutTable::rows().
//
// NOTHING IS WRITTEN BY HAND HERE, and that is the entire reason phase 3
// replaced keyPressEvent's flat switch with a table months before there was a
// window to render it into: a switch cannot be enumerated, grouped or printed,
// so the alternative was a second hand-maintained list of the same keys. Rows
// that carry a QAction point AT it rather than copying its keys, so a rebinding
// updates this window without anyone remembering to.
void MainWindow::showKeyboardShortcuts() {
    if (!shortcutsWindow_) {
        shortcutsWindow_ = new trace::app::ShortcutsWindow(shortcuts_, this);
    }
    shortcutsWindow_->show();
    shortcutsWindow_->raise();
    shortcutsWindow_->activateWindow();
}

// Always on Top, through the z-order band rather than through the window flags.
//
// setWindowFlag(Qt::WindowStaysOnTopHint) is the obvious implementation and it
// is wrong here: Qt destroys and recreates the native window when a top-level
// widget's flags change, and the D3D11 swapchain's surface is a child HWND
// created once from the viewer's winId(). SetWindowPos moves the window between
// the topmost and non-topmost bands and changes nothing about its identity, so
// the surface, the swapchain and every handle survive untouched.
//
// SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE: the position, the size and which
// window has focus are all somebody else's business. Only the band changes.
void MainWindow::applyAlwaysOnTop(bool on) {
#ifdef Q_OS_WIN
    auto hwnd = reinterpret_cast<HWND>(winId());
    if (!hwnd) return;
    SetWindowPos(hwnd, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
#else
    Q_UNUSED(on);
#endif
}

// EVERYTHING THAT MUST BE FORGOTTEN BEFORE THE NEXT THING HAPPENS, whether the
// next thing is a different file or nothing at all.
//
// This was the first sixty lines of openPath until spec phase 14, and Close
// Media is the reason it has a name: two call sites clearing "most of" the same
// state is how a stale audio ring or a keyframe grid learned from the previous
// file survives into the next one, and neither would be visible until some
// unrelated measurement came out wrong.
//
// It does NOT touch currentMedia_, currentImage_ or the viewer. The caller
// decides what replaces them -- openPath is about to assign new ones and
// closeMedia clears them -- and doing it here would mean openPath briefly
// having no media, which is a state its failure paths do not expect.
void MainWindow::releaseCurrentMedia() {
    // Before anything else touches the decoder. Bumps the generation, so a
    // frame the worker is producing for the OUTGOING media can never be
    // inserted against the incoming media, and waits for it to park so
    // close() below is not racing a decode.
    //
    // No landing decode: the outgoing media is about to be closed, and decoding
    // a frame of it here would be work thrown away at best and a frame of the
    // wrong file on screen at worst.
    endShuttleRun(/*landExactly=*/false);
    reclaimDecoder();
    scrubWorker_.stop();
    playTimer_.stop();
    stopAudio();
    userPlayIntent_ = false;
    audio_.close();
    scrubTimer_.stop();
    scrubbing_ = false;
    scrubJumpPending_ = false;
    scrubShownExact_ = false;
    scrubPaintGapLastMs_ = scrubPaintGapMaxMs_ = scrubPaintGapSumMs_ = 0.0;
    scrubPaintGapSamples_ = scrubPaintsWasted_ = scrubPaintStalls_ = 0;
    scrubPaintHitches_ = 0;
    stopUiServiceMeasurement();
    uiServiceGapMaxMs_ = uiServiceGapSumMs_ = 0.0;
    uiServiceSamples_ = uiServiceGapsOver_ = 0;
    scrubReleaseLatencyMs_ = 0.0;
    scrubLastPresentNs_ = -1;
    pendingScrubFrame_ = -1;
    activeScrubFrame_ = -1;
    // reclaimDecoder() above already cleared landingPending_, but it counted a
    // supersede while doing it and left the per-media figures standing. These
    // are per file, like the keyframe grid below: a landing latency carried
    // across an open would be a figure from the outgoing media.
    landingPending_ = false;
    landingKind_ = LandingKind::None;
    landingFrame_ = -1;
    landingGeneration_ = -1;
    landingStepDelta_ = 0;
    landingLatencyMs_ = landingLatencyMaxMs_ = 0.0;
    landingsAsync_ = landingsSync_ = landingsSuperseded_ = 0;
    // reclaimDecoder() above already drained the queue. These are the per-media
    // figures beside it: a starvation count carried across an open would be the
    // outgoing file's.
    stopPlaybackPrefetch();
    pqStarves_ = pqAheadDrops_ = pqReseeds_ = pqPosted_ = 0;
    pqMaxDepth_ = pqBytes_ = pqPeakBytes_ = 0;
    pqWaitMaxMs_ = 0.0;
    // Per file: a keyframe grid learned from the outgoing media would snap the
    // incoming one onto positions that are not keyframes in it.
    shuttleGop_ = 0;
    shuttleKfAnchor_ = -1;
    shuttleSnapping_ = false;
    shuttleAdvance_ = 1;
    playbackClock_.invalidate();
    playbackAccumulatorMs_ = 0.0;
    videoDecoder_.close();
    frameSource_.reset();
    videoFrameBuffer_ = trace::core::VideoFrame{};
    lastFrameHandoffMs_ = 0.0;
    avgFrameHandoffMs_ = 0.0;
    frameHandoffSamples_ = 0;
    // Re-seed per media: a 1080p estimate would let a 4K file walk far enough
    // to fall behind the pointer on its first drag.
    scrubWalkPerFrameMs_ = 1.0;
    // Learned from the seeks this media actually performs; assumed free until
    // then, so an all-intra file is never penalised for a property it lacks.
    mediaWalkFramesTotal_ = 0;
    mediaSeekCount_ = 0;
    mediaSeeksSeen_ = 0;

    // Cleared before anything can succeed, so a failed open cannot leave the
    // previous file's size attached to the next one.
    openedFileBytes_ = -1;
}

// File > Close Media. The empty state, reached deliberately rather than only by
// starting the application.
//
// TRACE HAS NEVER HAD "MEDIA WAS OPEN AND NOW NOTHING IS" -- it had "nothing
// has been opened yet", which looks identical and is not: everything below had
// a value that has to be given up, and the window has a shape that came from
// the media it no longer has.
void MainWindow::closeMedia() {
    if (!currentMedia_.has_value()) return;

    releaseCurrentMedia();

    currentMedia_.reset();
    currentImage_.reset();
    playbackAtEnd_ = false;
    playbackEndFrame_ = -1;
    hasSourceTimecode_ = false;
    // Empty rather than paused: paused implies a frame to be paused ON.
    playback_.resetForNewMedia(-1);

    // The view transform resets, because it resets on a media change and this
    // is a media change. Leaving a rotation in force over nothing would apply
    // it to whatever was opened next, which the phase 10 rule forbids.
    if (viewer_) {
        viewer_->setFrame(trace::core::VideoFrame{});
        viewer_->setViewTransform(trace::render::ViewTransform{});
        // Back to square pixels and no container rotation, and back to the
        // 16:9 minimum. Leaving a 9:16 floor behind would constrain the empty
        // window to a shape whose media is gone.
        viewer_->setSourceShape(1.0, 0);
        viewer_->setSourcePixelSize(QSize());
        viewer_->setFitToWindow();
        viewer_->setMinimumAspect(16.0 / 9.0);
    }
    syncViewTransformActions();
    syncViewScaleActions();

    // The window keeps the size it has. Section 4 shapes the window to the
    // MEDIA, and there is none -- resizing to some default here would move a
    // window the user had placed, to say nothing new.
    setWindowTitle(QStringLiteral("Trace"));
    statusBar()->showMessage(tr("No media open"), 2000);

    syncTransportBar();
    syncShareActions();
    syncTimeDisplayActions();
    syncMediaDependentActions();
    // The inspector is modeless and may be on screen. It already renders "No
    // media open." -- this is what makes it say so at the moment it becomes
    // true rather than at the next resize.
    refreshInspector();
    refreshHud("Close Media");
}

// LOOP, AND THE ONE PLACE "PLAYBACK REACHED THE END" IS DECIDED.
//
// Three sites in the tick reach that moment and each used to set playbackAtEnd_
// itself: the shuttle run running off the tail, a Playback decode that finds
// nothing left (the decoder exhausted before the frame count says it should
// be), and the target clamping to maxFrame. Before Loop they only had to agree
// on a flag. Now they have to agree on a BEHAVIOUR, and a wrap that happens at
// two sites of three is a file that loops except when it does not -- with which
// one you get depending on the codec, since only long-GOP media reaches the
// second site at all.
//
// Returns true when the playhead has been moved to the other end and playback
// should CONTINUE; false when the caller must stop as it always did. Returning
// false on a failed landing is deliberate: a wrap that could not decode its
// first frame must stop, not spin at the boundary.
bool MainWindow::loopWrap(int direction) {
    if (!loopEnabled_ || !frameSource_) return false;
    const long long maxFrame = playback_.state().maxFrame;
    // Nothing to loop: a still, or a one-frame sequence. Wrapping would
    // re-present the same frame forever and read as a hang.
    if (maxFrame <= 0) return false;

    const long long target = direction > 0 ? 0 : maxFrame;
    playback_.setCurrentFrame(target);

    // Step, not Playback: the wrap is a jump to the far end, and the frame is
    // being landed on rather than decoded in sequence. Same reasoning as the
    // Play-at-end rewind, and it reuses the same exact seek path.
    QString error;
    supersedeInFlightRequests();
    prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, direction, true);
    if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 2000);
        return false;
    }
    syncTransportBar();

    // AUDIO IS RESTARTED AFTER THE PLAYHEAD MOVES, NOT BEFORE, because
    // startAudioForPlayback takes its offset from the current frame. Started
    // first it would seek the device to the OLD position -- the end of the file
    // -- and the picture would run from the head against sound running out.
    // This is the same ordering the scrub-release resume needed and for the
    // same reason.
    stopAudio();
    startAudioForPlayback();

    // The wrap is a discontinuity: it paid a landing decode on the UI thread,
    // so the schedule is far past its deadline and every cadence figure spans a
    // gap that is not playback. Re-establishing the timeline restarts both.
    //
    // NOTE FOR MEASUREMENT: this resets the cadence counters, so a cadence run
    // taken with Loop on reports the last lap rather than the whole run. Leave
    // Loop off for cadence.ps1, as its controls already assume.
    beginPlaybackTimeline();

    ++loopWraps_;
    return true;
}

// THE PLAYBACK SPEED MENU, AND WHY IT IS NOT A SIXTH CALLER OF startShuttle.
//
// A shuttle press means "one rung up from wherever I am" and the ladder decides
// which; a menu item means "this rate", and there is no rung to compute. Routing
// the menu through startShuttle would have had to fake a press count to reach
// 10x, and would have been wrong at 0.5x in a way that looks right: stride is
// lround(speed), lround(0.5) is 1, so 0.5x would have satisfied the
// `ordinaryForwardPlay` predicate, started audio, and played at 1x while the
// menu ticked 0.5x.
//
// So it follows startShuttle's SEQUENCE -- which is the part that matters, and
// which section 29.2 is the record of getting wrong -- while choosing the rate
// itself. Steps 1, 4, 5, 6 and 7 below are that function's, in its order.
void MainWindow::setPlaybackSpeed(double speed) {
    if (!frameSource_ || !frameSource_->canPlay()) return;

    // 1. End the run in progress: a new rate invalidates everything already
    //    produced at the old one.
    endShuttleRun(/*landExactly=*/false);

    // Play from the end restarts the file, exactly as the Play action does --
    // otherwise picking a speed at the last frame reads as a dead menu, which
    // is the complaint `c3335ec` fixed for the Play button.
    if (playbackAtEnd_) {
        playback_.setCurrentFrame(0);
        playbackAtEnd_ = false;
        QString error;
        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 1, true);
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)
            && !error.isEmpty()) {
            statusBar()->showMessage(error, 3000);
        }
        syncTransportBar();
    }

    // 2. The controller owns the rate. Nothing here writes state_.speed.
    playback_.playForwardAt(speed);

    const double actual = playback_.state().speed;
    const bool ordinaryForwardPlay = std::abs(actual - 1.0) < 1e-4;

    // 3. Intent, not state -- the same rule startShuttle applies. Only 1x
    //    forward is worth restoring after a drag; 0.5x and the fast rungs are
    //    deliberate gestures, and resuming one at 1x would be the wrong answer.
    userPlayIntent_ = ordinaryForwardPlay;

    prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, 1, true);

    // 4. Audio. One call, which declines for everything that is not exactly 1x
    //    forward -- including 0.5x, and see audioShouldDrive for why that
    //    predicate had to be tightened before this menu could exist.
    startAudioForPlayback();

    // 5. The timeline, never a bare playTimer_.start().
    beginPlaybackTimeline();

    // 6. Above 1x the rate is a sampling STRIDE and the run is a shuttle. At
    //    exactly 1x it is ordinary playback on the validated audio-mastered
    //    path. And at 0.5x it is ordinary playback with a DOUBLED PERIOD --
    //    the tick divides the frame period by the speed, so every source frame
    //    is presented for two frame periods and none is skipped. That is the
    //    honest meaning of half speed for a review tool, and it costs the
    //    decoder less per second than 1x rather than more.
    if (actual > 1.0 + 1e-4) {
        startShuttleRun(1, static_cast<int>(std::lround(actual)));
    }

    // 7. The rate indicator, for every surface at once. Blank at 1x: ordinary
    //    playback has no rate to announce.
    flashRate(ordinaryForwardPlay ? QString()
                                  : QStringLiteral("%1x").arg(actual, 0, 'g', 3));

    syncPlaybackSpeedActions();
    syncTransportBar();
    refreshHud("Playback Speed");
}

// Ticked from the ENGINE's rate, never from the item that was clicked.
//
// The spec's requirement is that "the checked item must reflect the effective
// playback rate", and the difference matters in three cases that all really
// happen: Rewind puts the rate negative and no forward item should be ticked;
// K and Space put it at zero; and running off the end stops playback. A menu
// that remembered what it last set would claim 10x over a stopped file.
void MainWindow::syncPlaybackSpeedActions() {
    if (speedActions_.empty()) return;
    const auto st = playback_.state();
    const bool forward = st.mode == PlaybackMode::PlayingForward;
    QAction* match = nullptr;
    if (forward) {
        for (auto* action : speedActions_) {
            if (std::abs(action->data().toDouble() - st.speed) < 1e-4) {
                match = action;
                break;
            }
        }
    }
    // QActionGroup is exclusive, so unchecking every item needs the group
    // released first -- otherwise it refuses to leave nothing checked and the
    // stale item stays ticked over a paused file.
    if (speedGroup_) speedGroup_->setExclusive(match != nullptr);
    for (auto* action : speedActions_) action->setChecked(action == match);
    if (speedGroup_) speedGroup_->setExclusive(true);
}

// Everything that needs media open. ONE function, so a command added later has
// one place to be listed rather than a condition repeated at every menu -- and
// so "what does the application do with nothing open" has a single answer that
// can be read.
void MainWindow::syncMediaDependentActions() {
    const bool haveMedia = currentMedia_.has_value();
    if (closeMediaAction_) closeMediaAction_->setEnabled(haveMedia);
    if (copyFrameAction_) copyFrameAction_->setEnabled(haveMedia);
    for (auto* action : speedActions_) action->setEnabled(haveMedia);
    // Deliberately NOT beside the Copy Current Frame line above. Loop and Copy
    // Current Frame are separate commits so either can be reverted alone, and
    // two one-line additions on adjacent lines make `git revert` conflict on
    // whichever goes second -- the lines are independent, and git can only see
    // that they touch. The speed loop between them is pre-existing context, so
    // each hunk now has something stable on both sides.
    if (loopAction_) loopAction_->setEnabled(haveMedia);
    // The view transforms need a picture to transform; the aspect lock needs a
    // shape to lock to. Both are harmless with nothing open and both would be
    // commands that visibly do nothing, which is the showInfo failure.
    if (rotateLeftAction_) rotateLeftAction_->setEnabled(haveMedia);
    if (rotateRightAction_) rotateRightAction_->setEnabled(haveMedia);
    if (flipHorizontalAction_) flipHorizontalAction_->setEnabled(haveMedia);
    if (flipVerticalAction_) flipVerticalAction_->setEnabled(haveMedia);
    if (resetViewTransformAction_) resetViewTransformAction_->setEnabled(haveMedia);
    // Spec phase 15's four, through the function that also decides their ticks,
    // so "may this run" and "is this the state" are answered in one place.
    syncViewScaleActions();
}
// COPY CURRENT FRAME (spec phase 14, Edit menu).
//
// The spec hedges this one -- "only if safely supported" -- and the hedge is
// earned: since GATE C there is no RGB anywhere on the shipping path, so the
// obvious one-line implementation (put viewer_->frame().toQImage() on the
// clipboard) puts a NULL image on the clipboard on the default renderer and a
// real one under TRACE_RENDERER=cpu. That is the worst possible failure -- it
// works on the control and not on the shipping build.
//
// Two refusals, both deliberate:
//
//   - A PREVIEW-RESOLUTION FRAME IS REFUSED rather than copied. Mid-drag the
//     picture is a preview converted to the displayed size, which on 4K is a
//     fiftieth of the pixels. Copying it would hand the user something that
//     looks like their frame and is a quarter of its width, with nothing on
//     screen to say so. The rule is the standing one read the other way round:
//     fidelity is owed to the frame the user stops on, and a copy is a stop.
//   - No media, no frame: the action is disabled, and this checks anyway,
//     because a shortcut reaches an action a menu never showed.
void MainWindow::copyCurrentFrame() {
    if (!viewer_) return;

    const auto& frame = viewer_->frame();
    if (frame.isNull()) {
        statusBar()->showMessage(tr("No frame to copy"), 2000);
        return;
    }
    if (frame.previewRes) {
        statusBar()->showMessage(
            tr("Release the timeline first - the picture is a scrub preview"), 3000);
        return;
    }

    QImage image;
    QString error;
    if (!videoDecoder_.frameToRgbImage(frame, image, error)) {
        statusBar()->showMessage(
            error.isEmpty() ? tr("Could not copy the frame") : error, 3000);
        return;
    }

    // The USER's view transform is deliberately NOT applied. A copy is of the
    // frame, at the resolution and orientation the file stores it in; rotate
    // and flip are temporary VIEWING state, which is what phase 10 called them
    // and what "new media resets transforms" means. Baking a session's rotation
    // into a copied frame would make the clipboard depend on when it was taken.
    QGuiApplication::clipboard()->setImage(image);
    statusBar()->showMessage(
        tr("Copied frame %1 (%2 x %3)")
            .arg(playback_.state().currentFrame)
            .arg(image.width())
            .arg(image.height()),
        2500);
}


// One exact seek, shared by both prompts. Goes through the same Step path a
// slider release uses, which is the spec's "use the existing exact-frame seek
// path" -- and is why neither prompt needed any decoder work at all.
void MainWindow::goToFrame(long long frame, const char* action) {
    if (!frameSource_) return;
    const long long maxFrame = frameSource_->maxFrame();
    if (maxFrame < 0 || frame < 0 || frame > maxFrame) return;

    // A run in progress owns the decoder lease and the playhead; end it before
    // moving, and land exactly, because this IS a stop.
    endShuttleRun(/*landExactly=*/true);
    if (playTimer_.isActive()) {
        playTimer_.stop();
        stopAudio();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;
    }
    playback_.pause();
    // A jump is not playback, so the intent a scrub release would restore is
    // cleared -- the same rule stepping and J/K/L follow.
    userPlayIntent_ = false;
    playback_.setCurrentFrame(frame);
    supersedeInFlightRequests();
    prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 0, true);

    QString error;
    if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
    }
    syncTransportBar();
    if (viewer_) viewer_->revealOverlay();
    refreshHud(action);
}

// THE FULL File / Edit / View / Window / Help STRUCTURE (spec phase 14).
//
// Until this phase the menus were where each feature could be REACHED rather
// than where the spec puts them: Time Display and Share lived under File
// because phases 7 and 8 needed them reachable and the structure did not exist
// yet, and every one of those placements carried a comment saying so. This is
// the phase that pays that off, and the whole of it is `addAction` calls --
// every command already exists as a shared QAction, which is what phases 2
// through 13 were building toward.
//
// A menu ADDS actions and never defines them. The two exceptions below define
// theirs inline and are marked; everything else comes from setupSharedActions,
// setupTransportControls or setupWindowActions.
//
// TWO ITEMS THE SPEC NAMES ARE DELIBERATELY ABSENT, and their absence is the
// honest state rather than an oversight:
//
//   - Check for Updates. The spec conditions it on "only if an updater exists"
//     and none does. Owner decision, 2026-08-11: omit it rather than ship a
//     greyed row -- the same reasoning that left phase 13's Window menu holding
//     one item.
//   - Actual Size / Fit to Window / Zoom In / Zoom Out. They change the FIT,
//     which drives the scrub preview size and therefore cache depth, and Actual
//     Size on 4K media puts the picture larger than the viewport, which needs a
//     pan model Trace has never had. The phase 14 audit split them out as
//     phase 15 and the owner accepted the split. Ctrl+0 stays unclaimed until
//     then, exactly as phase 10 left it.
void MainWindow::setupMenus() {
    // ---- File ---------------------------------------------------------------
    auto* fileMenu = menuBar()->addMenu(tr("&File"));

    // A member rather than a local, so setupShortcuts() can list it: the
    // Keyboard Shortcuts window has to print Ctrl+O too, and a table that can
    // only see the actions someone remembered to hoist is the second
    // hand-written list it exists to avoid.
    openAction_ = new QAction(tr("&Open..."), this);
    openAction_->setShortcut(QKeySequence::Open);
    connect(openAction_, &QAction::triggered, this, &MainWindow::openFileDialog);
    fileMenu->addAction(openAction_);

    // Open Recent (spec phase 11), immediately under Open as the spec's File
    // menu lists it. Its contents are filled by rebuildRecentMenu(), which the
    // constructor calls once after loading the stored strings -- NOT on
    // aboutToShow. See rebuildRecentMenu for why that distinction is the whole
    // performance requirement.
    recentMenu_ = fileMenu->addMenu(tr("Open &Recent"));

    fileMenu->addAction(closeMediaAction_);
    fileMenu->addSeparator();

    // Share (spec phase 8), the same submenu object the two transport buttons
    // pop. The spec's Menus section gives Share no top level of its own, so it
    // stays under File -- which is where it has been since phase 8, and this
    // time by the structure rather than for want of one.
    //
    // This is also the ONLY keyboard-reachable Share surface, and after phase 6
    // that matters more than it reads. The floating overlay has no widget tree,
    // so its Share button is invisible to a screen reader; a real QMenu in the
    // menu bar is not. The proxy tree below changes that, but the menu remains
    // the surface that works with no proxy at all.
    fileMenu->addMenu(shareMenu_);

    fileMenu->addSeparator();
    // DEFINED HERE, not in setupSharedActions, and it is one of the two
    // exceptions: Quit has exactly one surface and no state to synchronise.
    //
    // NO SHORTCUT, and it used to carry QKeySequence::Quit. That standard key
    // has no Windows binding, and Qt 6.10 renders the unbound sequence as the
    // literal word "Exit" -- so the menu read `Exit    Exit`, advertising a key
    // no keyboard has. It was invisible until phase 14 put the item in a menu
    // anyone reads. Alt+F4 closes the window and needs no declaration; the
    // Keyboard Shortcuts window is generated from the table, so an imaginary
    // binding would have been printed there too.
    auto* quitAction = new QAction(tr("E&xit"), this);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(quitAction);

    // ---- Edit ---------------------------------------------------------------
    auto* editMenu = menuBar()->addMenu(tr("&Edit"));
    editMenu->addAction(copyFrameAction_);
    editMenu->addSeparator();
    editMenu->addAction(rotateLeftAction_);
    editMenu->addAction(rotateRightAction_);
    editMenu->addSeparator();
    editMenu->addAction(flipHorizontalAction_);
    editMenu->addAction(flipVerticalAction_);
    editMenu->addSeparator();
    editMenu->addAction(resetViewTransformAction_);

    // ---- View ---------------------------------------------------------------
    auto* viewMenu = menuBar()->addMenu(tr("&View"));
    viewMenu->addAction(fullscreenAction_);
    viewMenu->addAction(alwaysOnTopAction_);
    viewMenu->addSeparator();

    // Playback Speed. Explicit numeric labels only, as the spec requires --
    // "Fast" and "Faster" are exactly what a review tool must not say. The
    // checked item is written by syncPlaybackSpeedActions from
    // playback_.state().speed, never by the handler that set it, so the menu
    // reports the rate in force rather than the rate last asked for.
    //
    // NEGATIVE RATES ARE NOT HERE, by the spec's own instruction: reverse is
    // Rewind's, and duplicating every rung with a minus sign would be a second
    // way to command the one rate machine.
    auto* speedMenu = viewMenu->addMenu(tr("Playback &Speed"));
    speedGroup_ = new QActionGroup(this);
    speedGroup_->setExclusive(true);
    struct SpeedRung { double speed; const char* label; };
    // 0.5x first, then Normal, then the shuttle ladder. The ladder above 1x is
    // PlaybackController's kShuttleLadder verbatim; 0.5x is the one rung that
    // did not exist before this phase.
    // Mnemonics: 0, N, 2, 5, X, 3 -- all distinct. 10x takes the `x` rather
    // than the obvious `0`, which 0.5x already has; that collision is the third
    // this phase produced and the reason warnOnDuplicateMnemonics() exists.
    static constexpr SpeedRung kRungs[] = {
        {0.5, QT_TR_NOOP("&0.5x")},
        {1.0, QT_TR_NOOP("&Normal - 1x")},
        {2.0, QT_TR_NOOP("&2x")},
        {5.0, QT_TR_NOOP("&5x")},
        {10.0, QT_TR_NOOP("10&x")},
        {30.0, QT_TR_NOOP("&30x")},
    };
    for (const auto& rung : kRungs) {
        auto* action = new QAction(tr(rung.label), this);
        action->setCheckable(true);
        action->setData(rung.speed);
        speedGroup_->addAction(action);
        speedMenu->addAction(action);
        speedActions_.push_back(action);
        const double speed = rung.speed;
        connect(action, &QAction::triggered, this, [this, speed]() { setPlaybackSpeed(speed); });
    }

    // Time Display, where the spec's View menu names it. All six actions were
    // created in setupSharedActions at phase 7; only their home moves.
    // "Time Displa&y", not "&Time Display": T is Always on Top's, one item up.
    auto* timeMenu = viewMenu->addMenu(tr("Time Displa&y"));
    timeMenu->addAction(timeDisplayFrameAction_);
    timeMenu->addAction(timeDisplaySecondsAction_);
    timeMenu->addAction(timeDisplayElapsedAction_);
    timeMenu->addAction(timeDisplayTimecodeAction_);
    timeMenu->addSeparator();
    timeMenu->addAction(goToFrameAction_);
    timeMenu->addAction(goToTimecodeAction_);

    viewMenu->addAction(loopAction_);
    viewMenu->addSeparator();

    // Spec section 4's aspect lock. DEFINED HERE and it is the second
    // exception, because its handler is the only consumer and it writes the
    // settings key it reads -- splitting the two apart would put the default
    // and the persistence in different files.
    lockAspectAction_ = new QAction(tr("&Lock Window to Media Aspect Ratio"), this);
    lockAspectAction_->setCheckable(true);
    // CHECKED BY DEFAULT, as the spec requires -- and the default is what the
    // settings read falls back to, so a fresh installation and one whose INI
    // predates this key behave identically rather than differing by which
    // version wrote the file.
    lockAspectAction_->setChecked(
        trace::app::settings().value(QLatin1String(kLockAspectKey), true).toBool());
    connect(lockAspectAction_, &QAction::toggled, this, [this](bool on) {
        trace::app::settings().setValue(QLatin1String(kLockAspectKey), on);
        // Turning it ON reshapes immediately, so the tick and the window agree
        // at once rather than at the next resize. Turning it OFF changes
        // nothing: "the user may freely resize" means leaving the window where
        // they put it, and snapping it to the ratio on the way out would be the
        // opposite of unlocking.
        if (on) applyMediaWindowShape();
        refreshHud(on ? "Lock aspect" : "Unlock aspect");
    });
    viewMenu->addAction(lockAspectAction_);
    viewMenu->addSeparator();

    // Spec phase 15's group, where the spec's own View list puts it: after
    // Always on Top and before the rest. It lands lower than that here because
    // the spec's list is an enumeration rather than an ordering, and grouping
    // the four scaling commands together next to the aspect lock they interact
    // with reads better than splitting them across the menu.
    viewMenu->addAction(actualSizeAction_);
    viewMenu->addAction(fitToWindowAction_);
    viewMenu->addAction(zoomInAction_);
    viewMenu->addAction(zoomOutAction_);
    viewMenu->addSeparator();
    viewMenu->addAction(toggleHudAction_);

    // ---- Window -------------------------------------------------------------
    auto* windowMenu = menuBar()->addMenu(tr("&Window"));
    windowMenu->addAction(minimizeAction_);
    windowMenu->addAction(maximizeRestoreAction_);
    windowMenu->addSeparator();
    // The SAME two QActions the View menu holds, listed again because the
    // spec's Window menu names them -- phase 3's rule, and the reason it is a
    // rule: a second pair of actions doing the same thing would need their
    // checked state kept in step with the first pair, and the two would
    // eventually disagree about which one the picture is actually in.
    windowMenu->addAction(actualSizeAction_);
    windowMenu->addAction(fitToWindowAction_);
    windowMenu->addSeparator();
    windowMenu->addAction(inspectorAction_);

    // ---- Help ---------------------------------------------------------------
    auto* helpMenu = menuBar()->addMenu(tr("&Help"));
    helpMenu->addAction(traceHelpAction_);
    helpMenu->addAction(keyboardShortcutsAction_);
    helpMenu->addSeparator();
    helpMenu->addAction(reportIssueAction_);
    helpMenu->addSeparator();
    helpMenu->addAction(aboutAction_);

    syncPlaybackSpeedActions();
    syncMediaDependentActions();
    warnOnDuplicateMnemonics();
}

// TWO ITEMS IN ONE MENU SHARING A MNEMONIC MAKES THE KEY CYCLE THE HIGHLIGHT
// INSTEAD OF ACTIVATING EITHER -- a menu that quietly stops responding to its
// own underlined letter.
//
// Phase 10 hit it once, between Rotate Right and Reset, and fixed it by
// inspection. Phase 14 built the full five-menu structure and introduced TWO in
// the View menu alone: Time Display took T from Always on Top, and Loop took L
// from Lock Window to Media Aspect Ratio. Neither is visible in a screenshot --
// the underlines all render, and the key simply does the wrong thing -- and
// both were found by a harness trying to drive the menu rather than by anyone
// reading it.
//
// So the third time it is a check rather than a habit. It walks the real menu
// bar after it is built, which means it sees what a user sees, including items
// added by a later phase that never read this comment.
//
// A WARNING, NOT AN ASSERT. A duplicated mnemonic is a usability defect and not
// a corruption; failing the launch over one would be worse than the defect. It
// goes to stderr through fprintf for the reason TRACE_SHAPE_LOG does: in this
// GUI-subsystem build Qt's message handler does not reliably reach a console.
void MainWindow::warnOnDuplicateMnemonics() const {
    const auto scan = [](const QList<QAction*>& actions, const QString& menuName) {
        QHash<QChar, QString> seen;
        for (const QAction* action : actions) {
            if (action->isSeparator()) continue;
            const QString text = action->text();
            const int amp = text.indexOf(QLatin1Char('&'));
            // Trailing '&', or "&&" which is a literal ampersand rather than a
            // mnemonic -- the M&M asset name is why that case is real here.
            if (amp < 0 || amp + 1 >= text.size()) continue;
            const QChar key = text.at(amp + 1).toLower();
            if (key == QLatin1Char('&')) continue;
            const auto it = seen.constFind(key);
            if (it != seen.constEnd()) {
                fprintf(stderr,
                        "trace-menu: DUPLICATE MNEMONIC '&%s' in the %s menu -- "
                        "\"%s\" and \"%s\". The key will cycle the highlight "
                        "instead of activating either.\n",
                        qPrintable(QString(key)), qPrintable(menuName),
                        qPrintable(it.value()), qPrintable(text));
            } else {
                seen.insert(key, text);
            }
        }
    };

    if (!menuBar()) return;
    // The menu bar's own titles, then each menu's items and each submenu's.
    scan(menuBar()->actions(), QStringLiteral("menu bar"));
    for (const QAction* top : menuBar()->actions()) {
        QMenu* menu = top->menu();
        if (!menu) continue;
        QString name = top->text();
        name.remove(QLatin1Char('&'));
        scan(menu->actions(), name);
        for (const QAction* item : menu->actions()) {
            if (QMenu* sub = item->menu()) {
                QString subName = item->text();
                subName.remove(QLatin1Char('&'));
                scan(sub->actions(), name + QStringLiteral(" > ") + subName);
            }
        }
    }
}

void MainWindow::setupTransportControls() {
    // The QActions carry the transport behavior and are shared by the
    // transport bar, menus, and keyboard. The bar only emits intent; nothing
    // about playback logic lives in the UI widget.
    // ONE exact-frame-step command per direction, reached by the button and by
    // the arrow key alike. Before spec phase 3 there were two near-copies of it
    // and they had drifted -- see stepOneFrame for the measured consequence.
    prevFrameAction_ = new QAction("Previous Frame", this);
    connect(prevFrameAction_, &QAction::triggered, this,
            [this]() { stepOneFrame(-1, "Prev Frame"); });

    playPauseAction_ = new QAction("Play", this);
    connect(playPauseAction_, &QAction::triggered, this, [this]() {
        togglePlayPause();
        refreshHud("Play/Pause");
    });

    nextFrameAction_ = new QAction("Next Frame", this);
    connect(nextFrameAction_, &QAction::triggered, this,
            [this]() { stepOneFrame(1, "Next Frame"); });

    // Spec phases 4 and 5. The two visible side controls are Fast-forward and
    // Rewind now, and they are the third and fourth callers of startShuttle --
    // which is why phase 3 extracted that sequence before either button existed
    // rather than after.
    //
    // AtTwoX, not the keyboard's AtOneX. The spec states it directly ("If
    // paused, the first press begins forward playback at +2x", and the same
    // sentence for Rewind at -2x) and the owner confirmed both readings on
    // 2026-08-10: the keyboard ladder and the button ladder differ at the first
    // rung and agree everywhere above it. The difference is an ARGUMENT to the
    // one rate machine, never a call site reaching past the controller to write
    // `speed`.
    fastForwardAction_ = new QAction("Fast-forward", this);
    connect(fastForwardAction_, &QAction::triggered, this, [this]() {
        startShuttle(1, trace::core::ShuttleEntry::AtTwoX);
        refreshHud("FF");
    });

    rewindAction_ = new QAction("Rewind", this);
    connect(rewindAction_, &QAction::triggered, this, [this]() {
        startShuttle(-1, trace::core::ShuttleEntry::AtTwoX);
        refreshHud("REW");
    });

    // The transport bar owns the slider widget; MainWindow keeps driving it,
    // so every scrub/seek path below is unchanged.
    transportBar_->setFrameText(QStringLiteral("--"));
    connect(transportBar_, &trace::ui::TransportBar::rewindClicked,
            rewindAction_, &QAction::trigger);
    connect(transportBar_, &trace::ui::TransportBar::playPauseClicked,
            playPauseAction_, &QAction::trigger);
    connect(transportBar_, &trace::ui::TransportBar::fastForwardClicked,
            fastForwardAction_, &QAction::trigger);
    // The same action the File menu and the shortcuts run. trigger() rather than
    // a lambda: a checkable action must flip its own tick, and the button and the
    // menu must not be able to disagree about which state that is.
    connect(transportBar_, &trace::ui::TransportBar::fullscreenClicked,
            fullscreenAction_, &QAction::trigger);
    // Spec phase 8. The bar computes where its own button is; the menu is the
    // same object the overlay and the menu bar use, so there is one Share menu
    // in the application reached from three places.
    connect(transportBar_, &trace::ui::TransportBar::shareClicked,
            this, &MainWindow::showShareMenu);

    timelineSlider_ = transportBar_->timelineSlider();
    // Keyboard is reserved for transport (arrow-key stepping, J-K-L). If the
    // slider kept focus after a drag, arrows would move the slider instead of
    // stepping frames.
    timelineSlider_->setFocusPolicy(Qt::NoFocus);
    // A wheel notch over the groove is the one way into the valueChanged lambda
    // below that is not part of a drag: QSlider steps the value with no press
    // and no release, so nothing would ever restore playback afterwards and the
    // intent flag would outlive the gesture. It is a stepping gesture, so it
    // clears the intent -- the filter classifies it and then lets it through
    // unchanged, so wheel-to-step still works exactly as before.
    timelineSlider_->installEventFilter(this);
    connect(timelineSlider_, &QSlider::sliderPressed, this, [this]() {
        if (suppressSliderSignal_) return;
        // A drag needs the decoder, and a reverse run is holding the lease. End
        // it without a landing decode: the press is about to land its own frame
        // exactly, and paying for two landings would put a stale one in front of
        // the one the user pointed at.
        endShuttleRun(/*landExactly=*/false);
        scrubbing_ = true;
        scrubJumpPending_ = true;
        startUiServiceMeasurement();
        resetScrubLagModel();
        // Suspends the mechanism only. userPlayIntent_ is deliberately not
        // touched here or in valueChanged below, which is what makes a drag
        // interrupt playback rather than end it.
        playback_.pause();
        playTimer_.stop();
        stopAudio();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;
    });

    connect(timelineSlider_, &QSlider::sliderReleased, this, [this]() {
        if (suppressSliderSignal_) return;
        scrubbing_ = false;
        stopUiServiceMeasurement();

        if (isVideoScrubActive()) {
            QElapsedTimer releaseTimer;
            releaseTimer.start();
            queueVideoScrubFrame(static_cast<long long>(timelineSlider_->value()));
            flushVideoScrub(true);
            scrubReleaseLatencyMs_ =
                static_cast<double>(releaseTimer.nsecsElapsed()) / 1'000'000.0;
            // After the landing, never before it: the exact frame is on screen,
            // full-res, and the decoder lease is back -- which playback needs,
            // because playback decodes synchronously on this thread.
            //
            // WHEN THE LANDING IS ASYNCHRONOUS, "after the landing" IS NO LONGER
            // HERE. The ordering rule is unchanged and its meaning is unchanged;
            // what moved is the moment it names. onScrubResult() reclaims and
            // resumes when the frame arrives. Returning here without resuming is
            // therefore the rule being obeyed, not skipped -- and the guard is
            // explicit rather than leaning on resumePlaybackAfterScrub()'s
            // pendingScrubFrame_ test, which happens to cover this case today
            // and would stop covering it the moment that test changed.
            if (landingPending_) {
                refreshHud("Scrub Release");
                return;
            }
            resumePlaybackAfterScrub();
            refreshHud("Scrub Release");
            return;
        }

        playback_.setCurrentFrame(static_cast<long long>(timelineSlider_->value()));
        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 1, true);
        QString error;
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
        } else if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
            prefetchNeighbors();
        }
        // Image sequences and stills take this branch instead, and need the
        // same restore: it is a separate code path, not a fallthrough.
        resumePlaybackAfterScrub();
        refreshHud("Scrub");
    });

    connect(timelineSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (suppressSliderSignal_) return;
        playback_.pause();
        playTimer_.stop();
        stopAudio();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;

        if (isVideoScrubActive()) {
            queueVideoScrubFrame(static_cast<long long>(value));
            return;
        }

        playback_.setCurrentFrame(static_cast<long long>(value));

        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 1, true);
        QString error;
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
        } else if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
            prefetchNeighbors();
        }
        refreshHud("Scrub");
    });
    syncTransportBar();
}

// Section 4: "returning from fullscreen, maximize or snap to normal mode ...
// reapply the media aspect lock". The same paragraph also says to restore the
// previous normal window position, and those two pull against each other --
// reshaping recentres, which throws away the position that was just restored.
//
// They only conflict when the restored geometry is already wrong, so that is
// what is tested. A window that left normal mode under the lock comes back at
// the right ratio and is left exactly where it was; one that does not -- the
// lock was switched on while maximized, or Windows imposed a size -- is
// reshaped. The tolerance is 1%, which is wider than the rounding a chrome
// measurement can introduce and far narrower than any real mismatch.
void MainWindow::changeEvent(QEvent* event) {
    QMainWindow::changeEvent(event);
    if (event->type() != QEvent::WindowStateChange) return;

    // Spec phase 14: the one Window-menu item whose TEXT is state. Written from
    // what the window manager actually did rather than from what the handler
    // asked for -- phase 6's rule for fullscreen, and it applies here for the
    // same reason: a maximize Windows declines (a size the work area cannot
    // hold, a snapped window) would otherwise leave the menu offering to
    // Restore a window that never maximized.
    if (maximizeRestoreAction_) {
        maximizeRestoreAction_->setText(isMaximized() ? tr("&Restore") : tr("Ma&ximize"));
    }

    if (!viewer_ || !windowGeometryIsOurs()) return;
    if (!lockAspectAction_ || !lockAspectAction_->isChecked()) return;
    const double wanted = currentDisplayAspect();
    if (!(wanted > 0.0) || viewer_->height() <= 0) return;
    const double have = static_cast<double>(viewer_->width()) / viewer_->height();
    if (std::abs(have - wanted) <= wanted * 0.01) return;
    applyMediaWindowShape();
}

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    ++resizeEvents_;
    syncScrubPreviewSize();
    // Spec phase 13. The inspector reports the current viewport size, so a
    // resize changes what it says. ARMING a single-shot is all that happens
    // here -- the refresh itself is deferred, because the size it has to read is
    // measured by the paint that has not run yet, and because a real corner drag
    // produces ~123 of these and the phase 12 record rules out building a panel
    // on every one of them. Returns immediately when the window is not open, so
    // the common case costs one null test.
    scheduleInspectorRefresh();
    // Fullscreen, maximize and Snap all arrive here as resizes too, so none of
    // them needs its own hook.
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == timelineSlider_ && event->type() == QEvent::Wheel) {
        userPlayIntent_ = false;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::syncScrubPreviewSize() {
    if (!viewer_) return;
    QElapsedTimer syncClock;
    syncClock.start();
    // Clears the frame cache inside the decoder, so it needs the decoder back.
    reclaimDecoder();
    // Device pixels, not logical: on a scaled display the widget is drawn at
    // more pixels than its logical size, and converting to the logical size
    // would put the softness back that this exists to remove.
    const double dpr = viewer_->devicePixelRatioF();
    const QSize px(static_cast<int>(std::lround(viewer_->width() * dpr)),
                   static_cast<int>(std::lround(viewer_->height() * dpr)));
    const int dropped = videoDecoder_.setScrubPreviewSize(px);
    if (dropped > 0 || px != lastPreviewPx_) {
        ++previewSizeChanges_;
        cacheEntriesDropped_ += dropped;
    }
    lastPreviewPx_ = px;
    const double ms = static_cast<double>(syncClock.nsecsElapsed()) / 1'000'000.0;
    syncPreviewMsTotal_ += ms;
    syncPreviewMsMax_ = std::max(syncPreviewMsMax_, ms);
}

double MainWindow::currentDisplayAspect() const {
    if (!viewer_ || !currentMedia_.has_value()) return 0.0;
    QSize pixels;
    if (currentMedia_->kind == MediaKind::VideoFile) {
        const auto& vm = videoDecoder_.metadata();
        pixels = QSize(vm.width, vm.height);
    } else if (currentImage_.has_value()) {
        // WIDTH AND HEIGHT, NOT image.size(), AND THIS WAS A REAL DEFECT.
        //
        // `LoadedImageInfo::image` is left default-constructed at BOTH sites
        // that build one in loadCurrentFrame -- only filePath, fileName,
        // extension, width, height and channels are ever filled -- so
        // `image.size()` is an empty QSize for every still and every image
        // sequence. This function then returned 0.0 at the isEmpty() test below,
        // and spec section 4's media-shaped window silently did nothing at all
        // for that whole media class.
        //
        // Found at spec phase 13, because the inspector reports current scale
        // from the same natural size and read "Unknown" on a still. It is
        // invisible from the window itself on 16:9 material, which is what a
        // by-observation check looks at: the 4096x2304 still opened with a
        // viewer of 1280x675 -- ratio 1.896 against the file's 1.7778 -- which
        // reads as a correctly-shaped window unless the numbers are compared.
        pixels = QSize(currentImage_->width, currentImage_->height);
    }
    if (pixels.isEmpty()) return 0.0;
    // Through the viewer, because the composition of media shape and session
    // transform lives there and must not be written a second time here.
    return viewer_->displayedAspect(pixels);
}

QSize MainWindow::windowChromeLogical() const {
    if (!viewer_) return QSize();
    // Force the pending layout before measuring. The HUD sets its own fixed
    // height from the number of lines it was given, and that invalidation is
    // not applied until the layout next runs -- so measuring straight after a
    // refreshHud that added a line reads the height from before it.
    if (auto* central = centralWidget(); central && central->layout()) {
        central->layout()->activate();
    }
    // Measured, not assembled from constants. The HUD's height depends on how
    // many lines the media produces, the docked bar is present or not depending
    // on TRACE_TRANSPORT_BAR, and the frame depends on the window state -- a sum
    // of named parts would be a list that goes stale the next time one of them
    // moves. The difference between the two widgets is all of it at once.
    return QSize(width() - viewer_->width(), height() - viewer_->height());
}

// SECTION 4'S EXCEPTIONS, IN ONE PREDICATE. Fullscreen, maximized and any
// Windows-imposed geometry (Snap Layouts) are states where "never fight Windows
// by continuously resizing a snapped or maximized window" applies, and where an
// exact no-black-bar guarantee cannot hold anyway. A snapped window reports
// neither fullscreen nor maximized, which is why the interactive-resize path is
// the only place the ratio is enforced: Snap resizes through SetWindowPos and
// sends no WM_SIZING at all, so declining to fight it is automatic rather than
// something this has to detect.
bool MainWindow::windowGeometryIsOurs() const {
    return !isFullScreen() && !isMaximized() && !isMinimized();
}

// SECTION 20.4, AND IT IS A FOUND BUG RATHER THAN A REGRESSION: this path had
// never executed, because the box had one display until 2026-08-14.
//
// Section 4 asks the window to "recalculate correctly when the window moves
// between monitors with different scaling". Nothing did. A DPI change is not a
// QEvent::WindowStateChange, so changeEvent's re-shape never ran; and it is not
// a drag, so it sends no WM_SIZING and constrainSizingRect's aspect lock never
// ran either. Windows scaled the window rect on its own and Trace accepted
// whatever came out.
//
// Measured on 4K H.264, primary 100% -> secondary 150%: the client lost 147
// LOGICAL pixels of height (1083 -> 936) while the width was preserved exactly,
// so the viewer stopped being the media's ratio and the picture pillarboxed --
// `display 1201x676` filling the viewer at open became `display 936x527` inside
// an unchanged `win 1201x934` after a round trip. The window was also 973
// logical tall against a work area of 672, i.e. far past section 4's 80% rule,
// because that rule is applied at open and open had happened on the other
// monitor.
//
// THE CONDITION IS THE SCALE FACTOR, NOT THE MONITOR. Moving between two
// monitors that share a scale factor needs no reshape, and resizing the window
// because the user dragged it somewhere would be its own surprise.
void MainWindow::reshapeAfterDpiChange() {
    if (!viewer_) return;
    const double dpr = viewer_->devicePixelRatioF();
    if (!(dpr > 0.0)) return;
    // Below the tolerance a 1.25 and a 1.2499999 are the same scale factor;
    // above it, any real Windows step (1.25/1.5/1.75/2.0) is caught.
    if (std::abs(dpr - lastShapeDpr_) < 0.01) return;
    lastShapeDpr_ = dpr;
    // Declines while fullscreen, maximized or minimized -- windowGeometryIsOurs.
    // A maximized window on the new monitor is already the right size for it and
    // Windows owns that geometry; changeEvent reshapes on the way back to
    // normal, which is the existing path for exactly this.
    if (!windowGeometryIsOurs()) return;
    ++dpiReshapes_;
    applyMediaWindowShape();
}

void MainWindow::applyMediaWindowShape() {
    if (!viewer_ || !windowGeometryIsOurs()) return;
    const double aspect = currentDisplayAspect();
    if (!(aspect > 0.0)) return;

    // The minimum has to follow the shape before the size is chosen, or the
    // resize below is clamped by a floor computed for the previous media.
    viewer_->setMinimumAspect(aspect);
    if (!lockAspectAction_ || !lockAspectAction_->isChecked()) return;

    // UP TO THREE PASSES, BECAUSE THE CHROME CANNOT BE MEASURED FROM A WINDOW
    // TOO SMALL TO SHOW IT.
    //
    // Chrome is `window height - viewer height`, which is only the chrome while
    // the layout can actually satisfy everything. When the window is smaller
    // than the layout's minimum -- the ordinary case at open, before the window
    // has been shaped -- the viewer is pinned at its own minimum instead and the
    // difference is whatever is left, not what the chrome needs. Measured on the
    // 4x5: chrome read 310 where it is really 407, so the window came out 97px
    // short and pillarboxed the picture inside it.
    //
    // Iterating fixes it without a hand-written list of "menu bar plus status
    // bar plus HUD plus transport bar", which is the alternative and is a list
    // that goes stale the next time one of those moves -- exactly what phase
    // 6's transport change would have broken. After the first pass the window is
    // large enough for the measurement to be honest, so the second agrees and
    // the third never runs. It converges or it stops; it cannot oscillate,
    // because each pass only ever reads what the layout did.
    for (int pass = 0; pass < 3; ++pass) {
        if (!applyMediaWindowShapePass(aspect, pass)) break;
    }
    // Section 20.4. Recorded HERE rather than only in reshapeAfterDpiChange, so
    // that the shape taken at open counts as a shape at this scale factor --
    // otherwise the first DPI change after launch would compare against 0.0 and
    // the reshape would look conditional when it is not.
    lastShapeDpr_ = viewer_->devicePixelRatioF();
}

// One pass. Returns true when the layout did not give the viewer the size that
// was asked for, i.e. when another pass is worth running.
bool MainWindow::applyMediaWindowShapePass(double aspect, int pass) {
    QScreen* screen = windowHandle() ? windowHandle()->screen() : QGuiApplication::primaryScreen();
    if (!screen) return false;
    const QRect work = screen->availableGeometry();
    const double dpr = viewer_->devicePixelRatioF();

    // Natural displayed size, in LOGICAL pixels. The source's pixels are device
    // pixels -- a 1920-wide frame should occupy 1920 physical pixels so the
    // image maps 1:1 to the panel -- so on a scaled display the logical size it
    // wants is smaller, not the same. This is the whole of "use DPI-aware window
    // calculations": getting it backwards puts a 4K frame in a window 1.5x too
    // big on a 150% display and then scales it down again.
    QSize natural;
    if (currentMedia_->kind == MediaKind::VideoFile) {
        natural = videoDecoder_.metadata().naturalDisplaySize();
    } else if (currentImage_.has_value()) {
        // Same defect as in currentDisplayAspect, and the two had to be fixed
        // together: an aspect ratio with no natural size to apply it to would
        // have gone on sizing the window from the layout's own hint.
        natural = QSize(currentImage_->width, currentImage_->height);
    }
    if (natural.isEmpty()) return false;
    // The user's transform can turn it on its side; the media's own rotation is
    // already inside naturalDisplaySize().
    if (viewer_->viewTransform().swapsAxes()) natural.transpose();

    // Everything that is not video, in both directions: the client chrome (menu
    // bar, status bar, the HUD's own height, the docked transport bar when it is
    // present) plus the window FRAME, which setGeometry does not include and
    // which still occupies work area. Leaving the frame out here is what makes a
    // window that "fits" overhang the taskbar by a title bar.
    const QSize chrome = windowChromeLogical();
    const QRect frameNow = frameGeometry();
    const QRect clientNow0 = geometry();
    const int frameW = frameNow.width() - clientNow0.width();
    const int frameH = frameNow.height() - clientNow0.height();

    // EVERY DECISION ABOUT THE SIZE IS MADE IN computeViewerSize, and none of it
    // is made here. That split is not tidiness: this function needs a window, a
    // screen and a layout, so it can only ever run at whatever scale factor the
    // machine is set to -- 100% on this box, where every dpr term is the
    // identity. The pure function takes dpr as an argument and is driven across
    // 1.00/1.25/1.50/2.00 by `Trace.exe --window-shape-selftest`, so the
    // shipping path and the tested path are the same code rather than two
    // implementations that agree today.
    trace::app::ShapeInputs in;
    in.naturalPixels = natural;
    in.aspect = aspect;
    in.dpr = dpr;
    in.chromeLogical = chrome;
    in.frameLogical = QSize(frameW, frameH);
    in.workAreaLogical = work.size();
    in.viewerMinimumLogical = viewer_->minimumSize();
    const trace::app::ShapeResult shape = trace::app::computeViewerSize(in);
    if (!shape.valid) return false;
    const int viewerW = shape.viewerLogical.width();
    const int viewerH = shape.viewerLogical.height();

    const QSize client(viewerW + chrome.width(), viewerH + chrome.height());

    // THE FRAME IS NOT THE CLIENT RECT, AND setGeometry TAKES THE CLIENT ONE.
    // On a top-level widget both geometry() and setGeometry() exclude the
    // window frame, so centring the client rect on the work area pushes the
    // TITLE BAR off the top of it -- measured at exactly -7px on this box, on
    // every shape, which looks like a rounding error and is a whole title bar.
    // Centre the frame, then set the client rect inset inside it.
    const QRect frame = frameGeometry();
    const QRect clientNow = geometry();
    const QMargins frameMargins(clientNow.left() - frame.left(), clientNow.top() - frame.top(),
                                frame.right() - clientNow.right(),
                                frame.bottom() - clientNow.bottom());
    const QSize outer(client.width() + frameMargins.left() + frameMargins.right(),
                      client.height() + frameMargins.top() + frameMargins.bottom());

    // Centred on the monitor the window is ALREADY on -- section 4's "do not
    // move the window unexpectedly to a different monitor" -- and clamped back
    // inside the work area, so a window that grew cannot end up with its title
    // bar somewhere the user cannot reach it.
    QRect target(QPoint(0, 0), outer);
    target.moveCenter(work.center());
    if (target.right() > work.right()) target.moveRight(work.right());
    if (target.bottom() > work.bottom()) target.moveBottom(work.bottom());
    if (target.left() < work.left()) target.moveLeft(work.left());
    if (target.top() < work.top()) target.moveTop(work.top());
    setGeometry(QRect(target.topLeft() + QPoint(frameMargins.left(), frameMargins.top()), client));

    // TRACE_SHAPE_LOG=1. Every term of the calculation and, crucially, what the
    // LAYOUT actually did with the result -- because the two disagreeing is the
    // failure mode here, and it is invisible from outside: a viewer that came
    // out wider than its own ratio pillarboxes the picture inside a window whose
    // whole purpose was to have no bars, and the window looks deliberate.
    // `win WxH` cannot show it, which is the same reason the settings home
    // needed its own log rather than a HUD field.
    // fprintf, not qWarning: in a GUI-subsystem build Qt's default handler does
    // not reliably reach a console's stderr, and the first version of this log
    // printed nothing at all while FFmpeg's own stderr messages came through
    // from the same run -- which reads as "the function never ran". Settings.cpp
    // reached the same conclusion at phase 11 and this follows it.
    if (!qgetenv("TRACE_SHAPE_LOG").isEmpty()) {
        const QString msg =
            QString("[shape] aspect %1 natural %2x%3 dpr %4 | chrome %5x%6 frame %7x%8 "
                       "scale %9 bound %10 | want viewer %11x%12 client %13x%14 | got viewer %15x%16 (%17)")
                   .arg(QString::number(aspect, 'f', 4))
                   .arg(natural.width()).arg(natural.height())
                   .arg(QString::number(dpr, 'f', 2))
                   .arg(chrome.width()).arg(chrome.height())
                   .arg(frameW).arg(frameH)
                   .arg(QString::number(shape.scale, 'f', 4))
                   .arg(QString::fromLatin1(trace::app::shapeBoundName(shape.bound)))
                   .arg(viewerW).arg(viewerH)
                   .arg(client.width()).arg(client.height())
                   .arg(viewer_->width()).arg(viewer_->height())
                   .arg(QString::number(viewer_->height() > 0
                                            ? static_cast<double>(viewer_->width()) / viewer_->height()
                                            : 0.0,
                                        'f', 4));
        fprintf(stderr, "[pass %d] %s\n", pass, msg.toLocal8Bit().constData());
        fflush(stderr);
    }

    // Did the layout actually give the viewer what was asked for? A mismatch
    // means the chrome measurement this pass was built on was taken from a
    // window too small to show the chrome, and the next pass -- measuring the
    // window this one just made -- will be right. Answered by asking the layout
    // rather than by predicting it, which is the only way to notice that a
    // widget nobody here knows about took some of the height.
    return viewer_->width() != viewerW || viewer_->height() != viewerH;
}

#ifdef Q_OS_WIN
// Reshapes the proposed outer rect so the VIDEO CLIENT AREA ends up at the
// media's ratio, moving only the edges the user is not dragging. Returns false
// -- changing nothing -- whenever the lock does not apply, so every decline is
// a no-op rather than a differently-shaped window.
//
// The rect is in PHYSICAL pixels and Qt's sizes are logical, so the chrome has
// to cross that boundary; on this box dpr is 1 and the conversion is the
// identity, which is exactly why it has to be written correctly rather than
// tested into place.
bool MainWindow::constrainSizingRect(RECT* rect, int edge) {
    if (!rect || !viewer_) return false;
    if (!lockAspectAction_ || !lockAspectAction_->isChecked()) return false;
    if (!windowGeometryIsOurs()) return false;
    const double aspect = currentDisplayAspect();
    if (!(aspect > 0.0)) return false;

    const double dpr = devicePixelRatioF();
    const QSize chromeLogical = windowChromeLogical();
    const int chromeW = static_cast<int>(std::lround(chromeLogical.width() * dpr));
    const int chromeH = static_cast<int>(std::lround(chromeLogical.height() * dpr));

    const int proposedW = rect->right - rect->left;
    const int proposedH = rect->bottom - rect->top;
    int videoW = proposedW - chromeW;
    int videoH = proposedH - chromeH;
    if (videoW <= 0 || videoH <= 0) return false;

    // Which axis the user is actually driving. The side edges give width, the
    // top and bottom give height, and a CORNER gives both -- so for a corner the
    // axis is whichever has moved further from where the drag started. Measured
    // against the START rect rather than the previous proposal, because a
    // per-message comparison can change its mind mid-drag and that reads exactly
    // like the oscillation this is here to avoid.
    bool widthDrives = true;
    switch (edge) {
        case WMSZ_LEFT:
        case WMSZ_RIGHT:
            widthDrives = true;
            break;
        case WMSZ_TOP:
        case WMSZ_BOTTOM:
            widthDrives = false;
            break;
        default: {
            const int dw = std::abs(proposedW - sizeMoveStartRect_.width());
            const int dh = std::abs(proposedH - sizeMoveStartRect_.height());
            widthDrives = dw >= dh;
            break;
        }
    }

    if (widthDrives) videoH = std::max(1, static_cast<int>(std::lround(videoW / aspect)));
    else videoW = std::max(1, static_cast<int>(std::lround(videoH * aspect)));

    // The viewer's minimum is aspect-correct now, so clamping to it cannot
    // itself break the ratio -- but the clamp has to drive the OTHER axis from
    // whichever one it moved, or a floor on one axis silently produces a
    // wrong-shaped window at the smallest size, which is where a user is most
    // likely to notice.
    const QSize floorLogical = viewer_->minimumSize();
    const int floorW = static_cast<int>(std::lround(floorLogical.width() * dpr));
    const int floorH = static_cast<int>(std::lround(floorLogical.height() * dpr));
    if (videoW < floorW || videoH < floorH) {
        videoW = std::max(videoW, floorW);
        videoH = std::max(1, static_cast<int>(std::lround(videoW / aspect)));
        if (videoH < floorH) {
            videoH = floorH;
            videoW = std::max(1, static_cast<int>(std::lround(videoH * aspect)));
        }
    }

    const int wantW = videoW + chromeW;
    const int wantH = videoH + chromeH;

    // Move only the edges that are NOT being dragged. This is the whole of
    // "the dragged edge or corner remains authoritative": the pointer stays
    // glued to the edge it grabbed, and the picture grows or shrinks away from
    // it. Adjusting the wrong side makes the window crawl out from under the
    // cursor, which is the classic symptom of an aspect lock written without
    // reading wParam.
    switch (edge) {
        case WMSZ_LEFT:
        case WMSZ_TOPLEFT:
        case WMSZ_BOTTOMLEFT:
            rect->left = rect->right - wantW;
            break;
        default:
            rect->right = rect->left + wantW;
            break;
    }
    switch (edge) {
        case WMSZ_TOP:
        case WMSZ_TOPLEFT:
        case WMSZ_TOPRIGHT:
            rect->top = rect->bottom - wantH;
            break;
        default:
            rect->bottom = rect->top + wantH;
            break;
    }
    return true;
}
#endif

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result) {
#ifdef Q_OS_WIN
    if (eventType == "windows_generic_MSG" && message) {
        MSG* msg = static_cast<MSG*>(message);
        switch (msg->message) {
            case WM_SIZING:
                ++wmSizing_;
                // THE CONSTRAINT GOES HERE, NOT IN resizeEvent, and section 4's
                // three separate requirements -- "the dragged edge or corner
                // remains authoritative", "adjust the other dimension smoothly",
                // "avoid resize-event recursion and visible oscillation" -- are
                // one requirement with one answer.
                //
                // Correcting after the fact is what PRODUCES oscillation: by the
                // time resizeEvent runs, Qt has laid out and painted a
                // wrong-shaped window, and the correction is itself a resize
                // that arrives as another resizeEvent. WM_SIZING hands over the
                // proposed rect BEFORE any of that happens, and its wParam names
                // the edge being dragged, so "authoritative edge" is read off
                // the message rather than inferred.
                if (constrainSizingRect(reinterpret_cast<RECT*>(msg->lParam),
                                        static_cast<int>(msg->wParam))) {
                    // TRUE, not the default FALSE: the documented contract for
                    // WM_SIZING is that a handler which modified the rect says
                    // so, and returning false here would leave the modified rect
                    // in place but let Qt's own handler run over it.
                    if (result) *result = TRUE;
                    return true;
                }
                break;
            case WM_ENTERSIZEMOVE: {
                ++wmEnterSizeMove_;
                RECT r{};
                if (GetWindowRect(reinterpret_cast<HWND>(winId()), &r)) {
                    sizeMoveStartRect_ = QRect(QPoint(r.left, r.top), QPoint(r.right - 1, r.bottom - 1));
                }
                inSizeMove_ = true;
                break;
            }
            case WM_EXITSIZEMOVE:
                ++wmExitSizeMove_;
                inSizeMove_ = false;
                // Measured at experiment 1: deferring the preview-size sync to
                // here saves NOTHING, because the drag discards one cache's
                // worth of entries however many times it clears -- nothing
                // refills the cache while the pointer is down. So nothing is
                // deferred to this message and it stays a counter. The bracket
                // is kept because the corner-drag axis choice needs the rect the
                // drag started from.
                break;
            // Not part of the design -- it is the CONTROL on the other three.
            // WM_SIZE fires on every resize, interactive or programmatic, so a
            // run where the window demonstrably changed size and this still
            // reads 0 says nativeEvent is not being reached at all, which looks
            // identical from the other three counters to a gesture that missed
            // the resize border. The first run of resizecache.ps1 read 0/0/0
            // and could not tell those apart.
            case WM_SIZE: ++wmSize_; break;
            // Section 20.4, and OBSERVED rather than handled: Qt is per-monitor
            // DPI aware v2 on Windows and does the whole response itself --
            // the suggested rect in lParam, the screenChanged signal, the
            // backing-store rebuild. Taking the message here would be writing a
            // second implementation of that.
            //
            // What it is for is that "a real WM_DPICHANGED has never arrived"
            // was a claim no number in this application could check. `dpiChg`
            // makes it a reading. The dpr is sampled BEFORE the default handler
            // runs, which is why `from` is meaningful -- afterwards there is
            // only one value and no way to know it moved.
            case WM_DPICHANGED: {
                ++wmDpiChanged_;
                lastDpiChangeFrom_ = viewer_ ? viewer_->devicePixelRatioF()
                                             : devicePixelRatioF();
                // LOWORD of wParam is the new DPI for this window, from the
                // message itself rather than from a widget that has not been
                // updated yet.
                lastDpiChangeTo_ = static_cast<double>(LOWORD(msg->wParam)) / 96.0;
                // Section 4's "recalculate correctly when the window moves
                // between monitors with different scaling". Deferred, because
                // Qt has not applied the new scale factor or relaid out yet and
                // the shaping pass measures the layout -- see the timer's
                // declaration. Restarted rather than started, so a crossing that
                // produces more than one message still reshapes once.
                dpiReshapeTimer_.start();
                break;
            }
            default: break;
        }
    }
#endif
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::syncPlanarOutput() {
    if (!viewer_) return;
    // The renderer is asked, rather than TRACE_RENDERER being consulted: a GPU
    // backend that failed to initialize has already been replaced by the CPU
    // one, and only the widget knows which is actually installed.
    //
    // TRACE_PLANAR_UPLOAD=0 forces the GATE B path back on for an A/B without a
    // rebuild -- the same shape as every other knob in this codebase, and the
    // control that says whether a difference is the planar path or the day.
    static const bool allowed = qgetenv("TRACE_PLANAR_UPLOAD") != "0";
    // Clears the decoder's frame cache, so it needs the decoder back.
    reclaimDecoder();
    videoDecoder_.setPlanarOutputEnabled(allowed && viewer_->rendererAcceptsPlanarYuv());
}

void MainWindow::syncTransportBar() {
    if (!timelineSlider_ || !playPauseAction_) return;

    const auto st = playback_.state();
    const int maxFrame = static_cast<int>(std::max(0LL, st.maxFrame));

    suppressSliderSignal_ = true;
    timelineSlider_->setMaximum(maxFrame);
    // While the user is holding the handle, the handle belongs to the user.
    //
    // This wrote the *decoded* frame back into the slider on every HUD
    // refresh, which during a drag is several times a second -- so the handle
    // was repeatedly yanked from under the pointer to wherever the picture had
    // got to, and the next mouse move dragged it back. That is precisely the
    // "slider not keeping up with the pull" report, and it is not event-loop
    // starvation: the handle was being moved somewhere else on purpose.
    //
    // It also corrupted the landing. sliderReleased lands on
    // timelineSlider_->value(), so if the last write before mouse-up was a
    // yank rather than a pointer move, the release lands on the frame the
    // decoder happened to reach instead of the one the user pointed at.
    if (!timelineSlider_->isSliderDown()) {
        timelineSlider_->setValue(static_cast<int>(std::clamp(st.currentFrame, 0LL, st.maxFrame < 0 ? 0LL : st.maxFrame)));
    }
    suppressSliderSignal_ = false;

    const bool hasPlayableRange = st.maxFrame > 0;
    const bool hasAnyMedia = st.maxFrame >= 0;
    const bool playing = st.mode == PlaybackMode::PlayingForward || st.mode == PlaybackMode::PlayingReverse;

    timelineSlider_->setEnabled(hasAnyMedia);
    prevFrameAction_->setEnabled(hasAnyMedia);
    nextFrameAction_->setEnabled(hasAnyMedia);
    // Both shuttle controls need somewhere to run, which a single still frame
    // is not.
    if (fastForwardAction_) fastForwardAction_->setEnabled(hasPlayableRange);
    if (rewindAction_) rewindAction_->setEnabled(hasPlayableRange);
    playPauseAction_->setEnabled(hasPlayableRange);
    playPauseAction_->setText(playing ? "Pause" : "Play");

    // Checked state comes from the window and from viewState_, never from
    // whichever surface was clicked last. Menu tick, button highlight and
    // shortcut therefore cannot drift apart.
    if (fullscreenAction_) fullscreenAction_->setChecked(isFullScreen());
    if (toggleHudAction_) toggleHudAction_->setChecked(viewState_.showHud);

    if (transportBar_) {
        transportBar_->setControlsEnabled(hasAnyMedia);
        transportBar_->setPlaying(playing);
        transportBar_->setFullscreen(isFullScreen());
        transportBar_->setFrameText(hasAnyMedia
            ? QStringLiteral("%1 / %2").arg(st.currentFrame).arg(std::max(0LL, st.maxFrame))
            : QStringLiteral("--"));
    }
}

void MainWindow::openFileDialog() {
    const QString filter = "Media (*.mp4 *.mov *.png *.jpg *.jpeg *.tif *.tiff *.exr);;All Files (*.*)";
    const QString path = QFileDialog::getOpenFileName(this, "Open Media", {}, filter);
    if (!path.isEmpty()) openPath(path);
}

// Spec phase 11. Built from the stored strings and NOTHING ELSE.
//
// There is no QFileInfo here, and that is the requirement rather than an
// omission. The spec says not to probe every path during application startup
// and not to block on disconnected LucidLink or network paths, and this
// function is where both would happen: the obvious "grey out the ones that are
// gone" costs a stat per entry, which is 407ms apiece on a cold LucidLink mount
// and a multi-second SMB timeout on an unreachable UNC host. Ten entries of
// that in front of a menu the user has just clicked reads as the application
// hanging.
//
// Called when the list CHANGES -- once at startup, and on open/forget/clear --
// rather than on aboutToShow. Same cost either way today; the difference is
// that aboutToShow is the natural home for a later "just check quickly", and
// this is not.
void MainWindow::rebuildRecentMenu() {
    if (!recentMenu_) return;
    recentMenu_->clear();

    const QStringList& paths = recentFiles_.paths();
    for (int i = 0; i < paths.size(); ++i) {
        const QString& path = paths.at(i);
        // The name only -- a full path is unreadable in a menu and the whole
        // path is on the status tip. QFileInfo would be the obvious way to take
        // the basename and it is exactly what must not appear in this function,
        // so the separator search is done on the string.
        const int slash = std::max(path.lastIndexOf('\\'), path.lastIndexOf('/'));
        QString name = (slash >= 0) ? path.mid(slash + 1) : path;
        // A filename containing '&' would otherwise render as a mnemonic and
        // lose the character -- "R&D_v3.mov" would show as "RD_v3.mov" and
        // silently claim Alt+D.
        name.replace(QStringLiteral("&"), QStringLiteral("&&"));

        // The conventional numbering: 1-9 take a digit mnemonic, and the tenth
        // takes '0' rather than a second '1' that would collide with entry 1.
        const QString text = (i < 9) ? tr("&%1  %2").arg(i + 1).arg(name)
                                     : tr("1&0  %1").arg(name);
        QAction* entry = recentMenu_->addAction(text);
        // The full path where it can be read without being clicked. Both, so a
        // hover in either surface answers "which of the three v2s is this".
        entry->setStatusTip(path);
        entry->setToolTip(path);
        connect(entry, &QAction::triggered, this, [this, path]() { openRecentPath(path); });
    }

    recentMenu_->addSeparator();
    recentMenu_->addAction(clearRecentAction_);

    // Disabled rather than hidden when empty, which is the same rule phase 8
    // applied to the Share rows: a menu whose items come and go cannot be
    // learned, and a missing command reads as a broken build rather than as an
    // answer. Clear Recent Files goes grey with it, since there is nothing to
    // clear.
    const bool any = !recentFiles_.isEmpty();
    recentMenu_->setEnabled(any);
    clearRecentAction_->setEnabled(any);
}

// Spec phase 11. Opening an entry, and the missing-file case.
//
// NO PROBE BEFORE THE OPEN, deliberately. The tempting shape is "check it is
// there, then open it", and on a disconnected mount that check costs exactly
// what the open costs and then the open pays it again. Handing the path
// straight to openPath() means the recent list never makes Trace touch a path
// the user did not just ask it to, and an entry chosen here costs precisely
// what the same file chosen through File > Open costs.
//
// The existence question is asked only AFTER a failure, when the path has
// already been reached and answering is free -- and it is asked at all because
// "the file is gone" and "the file is there but will not decode" need different
// answers: only the first may offer to remove the entry.
void MainWindow::openRecentPath(const QString& path) {
    if (openPath(path)) return;

    if (QFileInfo::exists(path)) {
        // openPath already reported the real reason on the status bar. Removing
        // an entry whose file is present would throw away a perfectly good
        // bookmark because of a transient decode failure.
        return;
    }

    // Reported, with an offer to remove -- the spec's wording, and not a silent
    // drop. Keep is the default button: the destructive option should not be
    // what a stray Return chooses.
    QMessageBox box(this);
    box.setIcon(QMessageBox::Warning);
    box.setWindowTitle(tr("Trace"));
    box.setText(tr("Trace can't find this file."));
    box.setInformativeText(path);
    // Mnemonics on both, which is the Windows convention for a message box and
    // is also what makes the choice reachable without a mouse -- the alpha's
    // accessibility story is keyboard-plus-menus (owner decision, 2026-08-11),
    // so a dialog whose only route to the non-default answer is a click would
    // be a hole in it.
    QPushButton* remove = box.addButton(tr("&Remove from Recent"), QMessageBox::DestructiveRole);
    QPushButton* keep = box.addButton(tr("&Keep"), QMessageBox::RejectRole);
    box.setDefaultButton(keep);
    box.exec();

    if (box.clickedButton() == remove) {
        recentFiles_.forget(path);
        rebuildRecentMenu();
        statusBar()->showMessage(tr("Removed from Recent Files."), 2500);
    }
}

long long MainWindow::supersedeInFlightRequests() {
    ++requestGeneration_;
    // A read already under way is never abandoned -- the destination buffer
    // belongs to FFmpeg and is being written -- it is run to completion and
    // reported stale. The generation bump is what makes the decode spanning it
    // get discarded.
    if (storageBusy_) videoDecoder_.cancelOutstandingIo();
    // Deliberately does NOT push the new generation at the scrub worker.
    //
    // This is called on every pointer move, and the shuttle's target is not
    // the pointer -- it is the next frame after the one on screen, which does
    // not change as the pointer travels. Telling the worker that its target
    // had moved every time the mouse did was measured: 111 abandoned walks and
    // 141 stale results out of 404 posted, seeks up from 28 to 118, cache hits
    // halved and only a third of the frames painted. The worker is superseded
    // where the target genuinely changes -- reclaimDecoder, which is release,
    // play, media switch and shutdown.
    return requestGeneration_;
}

void MainWindow::grantDecoderLease() {
    if (decoderLeased_) return;
    // The stall pump keeps the CALLING thread's event loop alive during a slow
    // remote read. On the worker there is no event loop to keep alive and the
    // widgets it touches belong to this thread, so it comes off for the
    // duration of the lease. A remote read simply blocks the worker instead,
    // which is the correct behaviour and the entire point of the exercise.
    videoDecoder_.setStallPump(nullptr);
    decoderLeased_ = true;
}

double MainWindow::reclaimDecoder() {
    if (!decoderLeased_) return 0.0;
    // A landing in flight is superseded by whatever is taking the decoder back,
    // and this is the one place that can be true. Clearing it HERE rather than
    // at the delivery boundary is what stops it dangling: a superseded result
    // is dropped by the generation test at the top of onScrubResult and never
    // reaches the landing branch, so a flag cleared only there would stay set
    // forever and the next landing would be filed against a stale generation.
    //
    // The gesture's completion is dropped with it, deliberately. A release
    // whose landing was superseded does not restore playback, because something
    // newer than that release now owns the state.
    if (landingPending_) {
        landingPending_ = false;
        landingKind_ = LandingKind::None;
        ++landingsSuperseded_;
    }
    // THE PLAYBACK PREFETCH DRAINS HERE AND NOWHERE ELSE, and putting it here
    // rather than at each transition is the whole of "deterministic
    // cancellation". Pause, stop, seek, scrub, step, shuttle start, file change,
    // end of media and shutdown all already funnel through this function --
    // loadCurrentFrame() calls it on entry, so every synchronous decode in the
    // application drains the queue automatically. A list of transitions would
    // have to stay complete; this cannot become incomplete.
    //
    // Section 29.2's lesson applied in advance: GATE E was validated on the Play
    // action alone, and for weeks every other path that started the timer
    // without establishing the timeline kept compiling silently.
    //
    // Clearing an empty queue is free, which is what makes it safe here.
    stopPlaybackPrefetch();
    // Order matters. Bump the generation FIRST: anything the worker publishes
    // between here and it parking is then already stale by construction, which
    // is what makes "no older preview can appear after the exact landing" a
    // property of the counter rather than of the ordering of two callbacks.
    scrubWorker_.supersede(supersedeInFlightRequests());
    const double waitMs = scrubWorker_.revokeLease();
    decoderLeased_ = false;
    videoDecoder_.setStallPump([this](double waitedMs) {
        pumpDuringStorageStall(waitedMs);
    });
    // The worker's last snapshot is whatever it published; refresh from the
    // live decoder now that reading it is this thread's business again.
    captureDecoderTelemetry();
    return waitMs;
}

// The exact landing, posted to the worker instead of decoded here.
//
// EXACTNESS IS UNCHANGED AND THAT IS THE WHOLE POINT OF THE OWNER'S RULING.
// The request carries RequestMode::Step, batch 1 and no time budget, so the
// worker calls the same decodeFrameAt with the same mode the UI thread called
// it with: full resolution, accurate conversion, the frame that was asked for.
// A landing is never cut short by the batch budget the way a drag slice is --
// `batchBudgetMs` is deliberately 0 here, because a budget on one frame could
// only ever mean "give up", and giving up on a landing is the one thing that
// is not allowed.
//
// LATEST-TARGET-WINS COMES FROM prepareVideoRequest, WHICH CALLS
// reclaimDecoder(). That bumps requestGeneration_ AND pushes it at the worker,
// so a preview mid-walk, a queued batch and an older landing are all stale from
// this line onward. It is the identical guarantee the synchronous landing got,
// taken at the identical moment -- loadCurrentFrame()'s own reclaimDecoder()
// was the first thing it did. Nothing about the ordering is new; only the
// thread that decodes afterwards is.
bool MainWindow::requestExactFrameAsync(long long frame, int direction, LandingKind kind) {
    if (!asyncLandingEnabled()) return false;
    // Shares the worker, so the drag's own control switch turns this off too.
    // Two knobs that can disagree about whether the worker is in use would be a
    // configuration in which the lease has no owner.
    if (!asyncScrubEnabled()) return false;
    // Video files only. An image sequence has no decoder, no worker and no
    // GOP -- its landing is a file read and was never the problem.
    if (!isVideoScrubActive()) return false;
    if (frame < 0) return false;
    // The UI thread is inside a decode of its own, because a remote read pumped
    // the event loop and something re-entered. Posting now would hand the
    // decoder to the worker while FFmpeg is mid-read on this thread. The caller
    // takes its existing deferral path instead.
    if (storageBusy_) return false;

    // ALREADY ON ITS WAY -- ADOPT IT RATHER THAN ASKING AGAIN.
    //
    // A click is press+release on the same value. Synchronously the press
    // landed before the release was delivered, so the release took
    // flushVideoScrub's scrubShownExact_ skip and cost nothing. With the
    // landing on the worker the release arrives while the press's walk is still
    // running, so that skip cannot fire -- and re-posting means
    // prepareVideoRequest's reclaimDecoder() CANCELS the walk and the fresh
    // request starts it again from the head of the file.
    //
    // Measured on the single-GOP Seedance clip before this: the click decoded
    // frame 82 twice, `sup 1`, `cancel max 128.43ms` of UI-thread wait at a
    // `ckpt 135.37ms` checkpoint granularity, and the picture arrived at 570ms
    // against the synchronous path's 555. The freeze was fixed and the latency
    // was made worse.
    //
    // The skip is the same skip, resting on the same fact -- the press asked
    // for this exact frame, at full resolution, through Step -- and differs only
    // in the frame being in flight rather than on screen. What transfers is
    // ownership of the completion: the release's resume now runs when this
    // lands.
    if (landingPending_ && landingFrame_ == frame) {
        landingKind_ = kind;
        landingStepDelta_ = 0;
        return true;
    }

    prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step,
                        direction, true);
    // The playhead moves at POST time, not on delivery, and the two disagree
    // for as long as the walk takes. That is deliberate and it matches what the
    // synchronous landing already did -- flushVideoScrub set the frame before
    // decoding it -- but it is worth naming, because this project treats
    // "display one frame and name it another" as a defect. The honest pair is
    // `target N | shown M`, which is written only on delivery, and the HUD's
    // `landing` field says a landing is outstanding. The counter states where
    // the user asked to be; target/shown state what arrived.
    playback_.setCurrentFrame(frame);

    grantDecoderLease();

    trace::core::ScrubRequest request;
    request.frame = frame;
    request.direction = direction;
    request.generation = requestGeneration_;
    request.mode = trace::core::VideoDecoderFFmpeg::RequestMode::Step;
    request.batch = 1;
    request.batchBudgetMs = 0.0;

    landingPending_ = true;
    landingFrame_ = frame;
    landingGeneration_ = request.generation;
    landingKind_ = kind;
    landingClock_.start();
    scrubInFlightDir_ = direction;
    ++landingsAsync_;
    scrubWorker_.post(request);
    return true;
}

// Present the landed frame. The reclaim and the gesture's completion action are
// the CALLER's, immediately after the drain loop -- reclaimDecoder() clears the
// result deque, and calling it from inside the loop that is draining that deque
// would silently discard anything else the worker had published.
void MainWindow::completeExactLanding(const trace::core::ScrubResult& result) {
    landingPending_ = false;
    if (landingClock_.isValid()) {
        landingLatencyMs_ =
            static_cast<double>(landingClock_.nsecsElapsed()) / 1'000'000.0;
        landingLatencyMaxMs_ = std::max(landingLatencyMaxMs_, landingLatencyMs_);
        // `release` MUST KEEP MEANING "HOW LONG UNTIL THE EXACT FRAME APPEARED".
        // sliderReleased times its own lambda, and with the landing on the
        // worker that lambda now returns in microseconds -- so left alone the
        // HUD would report a 520ms landing as 0.1ms and every harness reading
        // it would record a spectacular improvement that did not happen. It is
        // the same quantity as before, measured across the boundary the work
        // actually crossed. The win is in `ui gap max`, which is a different
        // field because it is a different claim.
        if (landingKind_ == LandingKind::ScrubRelease) {
            scrubReleaseLatencyMs_ = landingLatencyMs_;
        }
    }

    if (!result.ok) {
        if (!result.error.isEmpty()) statusBar()->showMessage(result.error, 3000);
        // A step that could not be decoded must not leave the counter claiming a
        // frame the viewer is not showing. The synchronous path put the playhead
        // back and re-landed; this puts it back and leaves the picture alone,
        // which is the same end state without a second decode in front of it.
        if (landingKind_ == LandingKind::Step && landingStepDelta_ != 0) {
            if (landingStepDelta_ < 0) playback_.stepForward();
            else                       playback_.stepBackward();
        }
        syncTransportBar();
        landingKind_ = LandingKind::None;
        landingStepDelta_ = 0;
        return;
    }

    videoFrameBuffer_ = result.frame;
    lastRequestedFrame_ = result.requestedFrame;
    lastDeliveredFrame_ = result.frame.frameIndex;
    playback_.setCurrentFrame(result.frame.frameIndex);
    viewer_->setFrame(videoFrameBuffer_);
    // repaint(), not update(). A landing is the frame the user stopped on and
    // it must be on screen when this returns, not whenever Qt next coalesces --
    // and the HUD's `display` and the view scale are measured BY the paint, so
    // a merely-scheduled one reports the previous state.
    viewer_->repaint();

    activeScrubFrame_ = result.frame.frameIndex;
    // Exact by construction: Step mode, full resolution, accurate conversion.
    // This is the flag the release reads to skip re-decoding a frame the press
    // already landed, so setting it on anything less would put a soft picture
    // on screen and call it the landing.
    scrubShownExact_ = true;

    // The tail of the synchronous landing, mirrored rather than simplified.
    //
    // Clearing pendingScrubFrame_ unconditionally looks right and is wrong: a
    // PRESS landing is not the end of the gesture. The pointer can move while
    // the landing is in flight -- which on this file is a third of a second --
    // and queueVideoScrubFrame will have recorded where it went. Dropping that
    // target would strand the picture on the press point with the button still
    // down and the hand somewhere else. The synchronous path re-armed the
    // coalescing timer in exactly this case and so does this.
    if (landingKind_ == LandingKind::ScrubRelease || !scrubbing_) {
        pendingScrubFrame_ = -1;
    } else if (pendingScrubFrame_ != activeScrubFrame_) {
        scrubTimer_.start(kScrubCoalesceMs);
    }
    syncTransportBar();
    landingKind_ = LandingKind::None;
    landingStepDelta_ = 0;
}

void MainWindow::captureDecoderTelemetry() {
    if (decoderLeased_) return;
    hudPerf_ = videoDecoder_.perfStats();
    for (int i = 0; i < static_cast<int>(trace::core::IoPhase::Count); ++i) {
        hudIo_[i] = videoDecoder_.ioStats(static_cast<trace::core::IoPhase>(i));
    }
}

bool MainWindow::openPath(const QString& path) {
    // Opening another file while storage is slow: supersede the outstanding
    // read so nothing from the previous media is presented afterwards. The
    // guard below then refuses to re-enter until that decode has unwound.
    //
    // Returns false, and Open Recent must not read that as "the file is gone" --
    // which is why it asks QFileInfo::exists() before offering to remove an
    // entry rather than treating any failure as a missing file.
    if (storageBusy_) {
        supersedeInFlightRequests();
        return false;
    }

    releaseCurrentMedia();

    trace::core::MediaItem item;
    item.path = path.toStdString();

    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();

    if (ext == "mp4" || ext == "mov") {
        QString err;
        // Installed before open() so even the probe reads, which on a cold
        // mount measured 407ms for a single read, cannot freeze the window.
        videoDecoder_.setStallPump([this](double waitedMs) {
            pumpDuringStorageStall(waitedMs);
        });
        if (videoDecoder_.open(path, err)) {
            // Audio is opened alongside but is never required: a picture-only
            // render must still open exactly as it did before.
            QString audioErr;
            if (!audio_.open(path, audioErr) && !audioErr.isEmpty()) {
                statusBar()->showMessage(audioErr, 3000);
            }
            item.kind = MediaKind::VideoFile;
            item.frameCount = videoDecoder_.metadata().frameCount;
            frameSource_ = std::make_unique<trace::core::VideoFrameSource>(&videoDecoder_);
            // Parked until a drag posts to it. Started per media so it never
            // holds a pointer to a decoder that has been closed.
            scrubWorker_.start(&videoDecoder_, this, [this]() { onScrubResult(); });
            playback_.resetForNewMedia(item.frameCount > 0 ? item.frameCount - 1 : -1);
            playback_.setCurrentFrame(0);
        } else {
            statusBar()->showMessage(err, 3000);
        }
    }

    if (!frameSource_) {
        const auto seq = trace::core::SequenceParser::detect(item.path);
        if (seq.has_value()) {
            item.kind = MediaKind::ImageSequence;
            item.sequence = seq;
            item.frameCount = static_cast<long long>(seq->frames.size());

            QStringList framePaths;
            framePaths.reserve(static_cast<int>(seq->frames.size()));
            const QString dir = QString::fromStdString(seq->directory);
            const QString prefix = QString::fromStdString(seq->prefix);
            const QString suffix = QString::fromStdString(seq->suffix);
            for (const int frameNumber : seq->frames) {
                const QString framePadded = QString("%1").arg(frameNumber, seq->padWidth, 10, QChar('0'));
                framePaths.push_back(dir + "/" + prefix + framePadded + suffix);
            }
            frameSource_ = std::make_unique<trace::core::ImageSequenceFrameSource>(&stillLoader_, framePaths, 24.0);

            playback_.resetForNewMedia(item.frameCount - 1);
            const auto frameNum = trace::core::SequenceParser::extractFrameNumber(item.path);
            long long idx = 0;
            if (frameNum.has_value()) {
                const auto& frames = seq->frames;
                const auto it = std::find(frames.begin(), frames.end(), *frameNum);
                if (it != frames.end()) idx = static_cast<long long>(std::distance(frames.begin(), it));
            }
            playback_.setCurrentFrame(idx);
        } else {
            item.kind = MediaKind::StillImage;
            item.frameCount = 1;
            frameSource_ = std::make_unique<trace::core::ImageSequenceFrameSource>(&stillLoader_, QStringList{path}, 24.0);
            playback_.resetForNewMedia(0);
            playback_.setCurrentFrame(0);
        }
    }

    currentMedia_ = item;
    // THE FILE SIZE FOR MEDIA THAT DOES NOT GO THROUGH THE DECODER, TAKEN HERE
    // AND ONLY HERE (spec phase 13).
    //
    // A video's size arrives free: MediaIoSource reads it while opening the
    // file, and VideoPerfStats::sourceBytes already carries it. A still or a
    // sequence frame has no MediaIoSource, so the one `QFileInfo` above -- which
    // this open already constructed to read the extension -- answers it. What
    // must never happen is a stat issued when the inspector is SHOWN: the spec
    // forbids blocking to compute optional values, and phase 11 measured an
    // unreachable UNC path at 21,037ms and a cold LucidLink read at 407ms.
    if (item.kind != MediaKind::VideoFile) openedFileBytes_ = fi.size();
    frameCache_.clear();
    frameCache_.setWindowCenter(playback_.state().currentFrame);

    // After frameSource_ exists and before the first HUD refresh. Parsing the
    // start timecode here rather than per refresh is the difference between one
    // parse per file and several per second during a drag; resetting the readout
    // here is what stops a file WITH timecode handing its mode to a file
    // WITHOUT one.
    refreshSourceTimecode();
    // Same shape and the same reason (spec phase 8): computed here so no
    // surface has to probe the filesystem to draw itself.
    refreshShareState();
    // Spec phase 14. Close Media, Copy Current Frame, Loop and every Playback
    // Speed rung need media open, and setupMenus() disabled them all at
    // construction -- correctly, since nothing was open then. Without this they
    // STAY disabled for the life of the process: the menu items render, the
    // shortcuts do nothing, and the failure is silent because a disabled
    // QAction does not report being triggered. Measured before it was added --
    // Ctrl+C put nothing on the clipboard and Edit > Copy Current Frame showed
    // no status message, which reads exactly like a broken conversion.
    syncMediaDependentActions();
    // Spec phase 10: "reset the transform when new media opens". A rotation the
    // user applied to inspect one clip must not silently follow them into the
    // next one, where it would look like the new file is tagged wrong -- which
    // in a review tool is a bug report about the media rather than about Trace.
    // Placed with the other per-open resets rather than inside applyViewTransform
    // so it is visibly part of what opening a file does.
    if (viewer_) {
        viewer_->setViewTransform(trace::render::ViewTransform{});
        syncViewTransformActions();
        // The MEDIA's own shape (spec phase 12), set immediately after the
        // user's transform is reset and never conflated with it. Reset to 1.0/0
        // for anything that is not a video file -- a still or an image sequence
        // has square pixels and no container rotation, and leaving the previous
        // clip's values in place would silently stretch the next thing opened.
        if (currentMedia_->kind == MediaKind::VideoFile) {
            const auto& vm = videoDecoder_.metadata();
            const double par = (vm.sarNum > 0 && vm.sarDen > 0)
                                   ? static_cast<double>(vm.sarNum) / static_cast<double>(vm.sarDen)
                                   : 1.0;
            viewer_->setSourceShape(par, vm.rotationDegrees);
            viewer_->setSourcePixelSize(QSize(vm.width, vm.height));
        } else {
            viewer_->setSourceShape(1.0, 0);
            // The still or sequence's own pixel size, from the same place
            // currentDisplayAspect() reads it. Phase 13's defect is the reason
            // this is not taken from LoadedImageInfo::image: that QImage is
            // left default-constructed at both sites that build one, so its
            // size is an empty QSize and section 4 silently did nothing for
            // this whole media class until 3a38516.
            viewer_->setSourcePixelSize(
                currentImage_.has_value()
                    ? QSize(currentImage_->width, currentImage_->height)
                    : QSize());
        }
        // Back to fit, because opening a file is a media change and a zoom is a
        // way of looking at ONE piece of media. Beside the transform reset for
        // the same reason that one is here: visibly part of what opening does,
        // rather than buried in the setter.
        viewer_->setFitToWindow();
    }
    syncViewScaleActions();

    prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 1, true);
    // Before the first frame: the decoder sizes scrub previews from this, and a
    // file opened into an already-sized window never gets a resize event to
    // tell it.
    syncScrubPreviewSize();
    syncPlanarOutput();

    QString error;
    if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
        refreshHud("Open failed");
        return false;
    }

    if (currentMedia_->kind == MediaKind::ImageSequence) prefetchNeighbors();

    const auto fps = frameSource_ ? std::max(1.0, frameSource_->fps()) : 24.0;
    // Video runs a short scheduler tick and decides presentation from the
    // playback accumulator, so the interval no longer quantizes the rate.
    // Image sequences keep the frame-rate interval and their existing
    // multi-step catch-up behaviour.
    // Measured on the 4K ProRes 4444 benchmark (261 frames, Release):
    //   periodic precise @ frame interval  11.00s  23.74fps  98.9%
    //   6ms poll + accumulator gate        11.34s  23.01fps  95.9%
    //   adaptive single-shot per frame     11.07s  23.57fps  98.2%
    // Decoupling the scheduler from the interval loses rather than gains: the
    // ~35ms blocking handler starves the event loop, so a short tick is only
    // delivered every ~13ms and polling for the due time costs latency a
    // periodic timer never pays. The residual gap is per-frame work, not
    // scheduler quantization, so the periodic timer stays.
    //
    // GATE E SUPERSEDES THE floor()-VERSUS-round() ARGUMENT THAT USED TO BE
    // HERE, and the reasoning is worth keeping because it explains why neither
    // choice could have worked.
    //
    // The old rule was floor(1000/fps), because round() puts the tick at 42ms
    // for a 41.71ms frame -- systematically slower than the frame rate, so
    // presentation could never keep up. floor() made the tick a *bound* and let
    // the playback clock choose which opportunities to use.
    //
    // But both are integers, and no integer divides 41.667. floor gave a 41ms
    // grid, so presents landed 41ms or 82ms apart and never 41.667: 61 frames
    // 1.6% fast, one held double, every 2.6s, on every file (plan section 23,
    // median long-gap spacing 61-62 across all six runs). The tick could not
    // be a bound AND a grid the presents land on, and it was both.
    //
    // The interval below is only a SEED. From the first tick of a run,
    // armNextPresent() re-arms per frame against an absolute deadline built
    // from the source's exact rational, so the arms alternate 41/42 and average
    // the true period. This is NOT the "adaptive single-shot per frame" row in
    // the table above: that one was measured on presented rate, which plan
    // section 23.1 established cannot see the beat at all, and it is unknown
    // whether it armed from an absolute deadline or a rounded period -- the
    // code is not in history. See plan section 24.7.
    playTimer_.setSingleShot(false);
    schedulerIntervalMs_ = std::max(1, static_cast<int>(std::floor(1000.0 / fps)));
    playTimer_.setInterval(schedulerIntervalMs_);
    // Qt defaults to a coarse timer above 20ms, which on Windows quantizes to
    // the ~15.6ms system tick. A precise timer is what makes a 6ms scheduler
    // tick meaningful at all.
    playTimer_.setTimerType(Qt::PreciseTimer);

    // Open Recent (spec phase 11). ONLY on success, and only for a file-backed
    // item -- a list of things that failed to open is not a recent-files list.
    //
    // The path comes from the Share gate, which canonicalised it a few lines
    // above as part of work this open was doing anyway. That is the reason
    // `canonicalNativePath` was exported rather than reimplemented: two answers
    // to "what is this file's path" would eventually disagree, and the recent
    // list is exactly where that shows up, as two rows for one file.
    if (shareState_.fileBacked && !shareState_.canonicalPath.isEmpty()) {
        recentFiles_.remember(shareState_.canonicalPath);
        rebuildRecentMenu();
    }

    statusBar()->showMessage("Opened", 1200);
    refreshHud("Open file");
    // THE SHAPE IS APPLIED HERE, AFTER refreshHud, AND THAT ORDER IS THE WHOLE
    // CORRECTNESS OF IT.
    //
    // The window is sized so that the VIDEO CLIENT AREA is the media's ratio,
    // and everything that is not video has to be subtracted first -- including
    // the HUD, whose height is the number of lines the media produces.
    // refreshHud is what sets those lines, so a shape applied before it
    // subtracts the PREVIOUS media's chrome, the HUD then grows and takes the
    // difference out of the viewer, and the picture is pillarboxed inside a
    // window that was supposed to eliminate bars. Measured on the 4x5: the
    // viewer came out 982x905 for a 0.8 ratio that wanted 724x905.
    applyMediaWindowShape();
    // "Update when active media changes" (spec). Armed rather than called: the
    // line above has just resized the window, so the paint that decides the
    // viewport size has not happened yet -- refreshing here would print the
    // PREVIOUS media's viewport into a panel about this one, and on a paused
    // file nothing would correct it. Phase 10's trap, and the reason the timer
    // is the only refresh route.
    scheduleInspectorRefresh();
    return true;
}

QString MainWindow::sequenceFramePath(long long frameIndex) const {
    if (!currentMedia_.has_value() || !currentMedia_->sequence.has_value()) return {};
    const auto& seq = *currentMedia_->sequence;
    if (frameIndex < 0 || frameIndex >= static_cast<long long>(seq.frames.size())) return {};

    const int frameNumber = seq.frames[static_cast<size_t>(frameIndex)];
    const QString dir = QString::fromStdString(seq.directory);
    const QString prefix = QString::fromStdString(seq.prefix);
    const QString suffix = QString::fromStdString(seq.suffix);
    const QString framePadded = QString("%1").arg(frameNumber, seq.padWidth, 10, QChar('0'));
    return dir + "/" + prefix + framePadded + suffix;
}

bool MainWindow::loadCurrentFrame(QString& error, trace::core::VideoDecoderFFmpeg::RequestMode mode) {
    // Re-entrancy: reached from a timer tick or key press delivered while the
    // event loop was being pumped inside another decode. One decode at a time.
    if (storageBusy_) return false;
    // Every synchronous decode -- step, playback, press landing, release
    // landing -- takes the decoder back first. This is the single choke point
    // that makes "the UI thread never touches a leased decoder" true rather
    // than a convention observed at a dozen call sites. It is free when no
    // lease is out, which is every frame of ordinary playback.
    reclaimDecoder();
    error.clear();
    if (!currentMedia_.has_value() || !frameSource_) {
        error = "No media selected";
        return false;
    }

    const long long frameIndex = playback_.state().currentFrame;
    frameSource_->setCurrentFrame(frameIndex);
    if (auto* videoSource = videoFrameSource()) {
        videoSource->setRequestMode(mode);
    }

    if (currentMedia_->kind == MediaKind::ImageSequence) {
        frameCache_.setWindowCenter(frameIndex);
        if (const auto cached = frameCache_.get(frameIndex); cached.has_value()) {
            trace::core::LoadedImageInfo info;
            info.filePath = cached->path;
            info.fileName = QFileInfo(cached->path).fileName();
            info.extension = QFileInfo(cached->path).suffix().toLower();
            info.width = cached->width;
            info.height = cached->height;
            info.channels = cached->channels;
            currentImage_ = info;
            viewer_->setFrame(cached->frame);
            syncTransportBar();
            return true;
        }
    }

    trace::core::VideoFrame decodedFrame;
    trace::core::VideoFrame* targetFrame = &decodedFrame;

    if (currentMedia_->kind == MediaKind::VideoFile) {
        targetFrame = &videoFrameBuffer_;
    }

    // Everything from here to the end of the decode may pump the event loop
    // (remote storage). Guard against re-entering the decoder from a timer
    // tick or a key press delivered by that pump, and clear the buffering
    // state on the way out however this returns.
    storageBusy_ = true;
    const auto clearStorageBusy = qScopeGuard([this]() {
        storageBusy_ = false;
        storageWaitMs_ = 0.0;
        if (buffering_) {
            buffering_ = false;
            if (bufferingClock_.isValid()) {
                bufferingMsTotal_ += static_cast<double>(bufferingClock_.elapsed());
            }
            if (overlay_) overlay_->setStorageState(QString());
        }
    });

    // The generation this request belongs to. Everything below may pump the
    // event loop, so the world can move underneath it.
    const long long generation = requestGeneration_;

    if (!frameSource_->frameAt(frameIndex, *targetFrame, error)) return false;

    // Superseded mid-request: the user dragged on, seeked, opened another file
    // or closed media while this was in flight. Presenting it now would put a
    // frame on screen that the user has already moved on from -- latest target
    // wins, and this is the one place that is enforced.
    if (requestGeneration_ != generation) {
        ++supersededResults_;
        error.clear();
        return false;
    }

    // What was asked for against what came back. The decoder resolves the
    // landed frame's identity from its PTS and stamps it on the frame, so this
    // is measured rather than assumed -- and it is measured for cache hits too,
    // which is what the HUD's target/shown could not previously report.
    lastRequestedFrame_ = frameIndex;
    lastDeliveredFrame_ = targetFrame->frameIndex;

    const QString sourcePath = frameSource_->sourcePathForFrame(frameIndex).isEmpty()
        ? QString::fromStdString(currentMedia_->path)
        : frameSource_->sourcePathForFrame(frameIndex);

    QElapsedTimer handoffTimer;
    handoffTimer.start();

    trace::core::LoadedImageInfo info;
    info.filePath = sourcePath;
    info.fileName = QFileInfo(sourcePath).fileName();
    info.extension = QFileInfo(sourcePath).suffix().toLower();
    info.width = targetFrame->width();
    info.height = targetFrame->height();
    info.channels = 4;

    currentImage_ = info;
    viewer_->setFrame(*targetFrame);

    lastFrameHandoffMs_ = static_cast<double>(handoffTimer.nsecsElapsed()) / 1'000'000.0;
    videoDecoder_.setHandoffTiming(lastFrameHandoffMs_);
    ++frameHandoffSamples_;
    const double handoffN = static_cast<double>(frameHandoffSamples_);
    avgFrameHandoffMs_ += (lastFrameHandoffMs_ - avgFrameHandoffMs_) / handoffN;

    if (currentMedia_->kind == MediaKind::ImageSequence) {
        trace::core::CachedFrame cf;
        cf.frameIndex = frameIndex;
        cf.path = info.filePath;
        cf.frame = *targetFrame;
        cf.width = info.width;
        cf.height = info.height;
        cf.channels = info.channels;
        frameCache_.put(cf);
    }

    syncTransportBar();
    return true;
}

void MainWindow::prefetchNeighbors() {
    if (!currentMedia_.has_value() || currentMedia_->kind != MediaKind::ImageSequence) return;

    const long long current = playback_.state().currentFrame;
    const long long neighbors[2] = {current - 1, current + 1};

    for (long long idx : neighbors) {
        const QString path = sequenceFramePath(idx);
        if (path.isEmpty()) continue;
        if (frameCache_.get(idx).has_value()) continue;

        trace::core::LoadedImageInfo info;
        QString error;
        if (!stillLoader_.load(path, info, error)) continue;

        trace::core::CachedFrame cf;
        cf.frameIndex = idx;
        cf.path = info.filePath;
        // `info` is about to go out of scope, so this moves rather than copies.
        cf.frame.buffer = trace::core::FrameBuffer::adopt(std::move(info.image));
        cf.frame.frameIndex = idx;
        if (!cf.frame.buffer) continue;
        cf.width = info.width;
        cf.height = info.height;
        cf.channels = info.channels;
        frameCache_.put(cf);
    }
}

void MainWindow::togglePlayPause() {
    // Space out of a reverse run is both a stop and a direction change, and it
    // lands: whichever branch below runs, it must start from the frame that was
    // on screen rather than from where the reverse pipeline had run ahead to.
    endShuttleRun(/*landExactly=*/true);

    if (!frameSource_ || !frameSource_->canPlay()) {
        playback_.pause();
        syncTransportBar();
        return;
    }

    if (playTimer_.isActive()) {
        playTimer_.stop();
        stopAudio();
        playback_.pause();
        userPlayIntent_ = false;
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;
    } else {
        // Play at the end restarts the file. Playback stops on the last frame
        // and leaves it there, so a second Play had nothing left to advance to
        // and the button read as dead. Rewinding here rather than in the tick
        // keeps it a property of the Play action: the playhead is only moved by
        // an explicit request to start, never on its own while stopping.
        if (playbackAtEnd_) {
            playback_.setCurrentFrame(0);
            playbackAtEnd_ = false;
            QString error;
            // Step, not Playback: this is a jump to a new position, and the
            // frame is being landed on rather than decoded in sequence.
            prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 1, true);
            if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)
                && !error.isEmpty()) {
                statusBar()->showMessage(error, 3000);
            }
            // The slider still reads the old position, and startAudioForPlayback
            // below takes its start offset from the current frame.
            syncTransportBar();
        }
        playback_.togglePlayPause();
        userPlayIntent_ = true;
        startPlaybackRun();
    }
    syncTransportBar();
}

// The caller has already put playback_ into the mode it wants. This starts the
// machinery for it and nothing else, so Play and resume-after-scrub run the
// same setup rather than two copies of it that drift.
void MainWindow::startPlaybackRun() {
    const int direction = playback_.state().mode == PlaybackMode::PlayingReverse ? -1 : 1;
    prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, direction, false);
    startAudioForPlayback();
    beginPlaybackTimeline();
    // Checkpoint 2 stage one, and it goes LAST because prepareVideoRequest calls
    // reclaimDecoder(), which drains the prefetch. Starting it any earlier in
    // this function would grant the lease and then immediately revoke it, and
    // the run would look exactly like one where the queue never engaged.
    //
    // Ordinary 1x forward only, and the direction test is the whole gate: a
    // reverse run is already queued by startShuttleRun, and anything off 1x is
    // a shuttle run rather than this path. startPlaybackPrefetch() refuses on
    // its own if a shuttle, a drag or a storage read owns the decoder.
    const auto st = playback_.state();
    if (direction > 0 && std::abs(st.speed - 1.0) < 1e-4) {
        startPlaybackPrefetch();
    }
}

// Everything a shuttle press does, in the one order that works.
//
// Extracted at spec phase 3, deliberately BEFORE phases 4 and 5 add the Rewind
// and Fast-forward buttons as further callers. It was five steps written out
// twice, in J and in L, and the two copies had already diverged in four places.
// Plan section 29.2 is why that matters more here than tidiness usually does:
// GATE E turned playback from a free-running timer into a timeline that must be
// ESTABLISHED, and a path that starts the timer without establishing it still
// compiles, still runs, and decays quadratically -- 20 presents in 8 seconds
// against a control's 111. A five-step sequence with no name is a sequence the
// next caller gets four steps of.
//
// ONE PREDICATE DECIDES THREE THINGS. `ordinaryForwardPlay` -- forward at
// exactly 1x -- is simultaneously the case that keeps the play intent a scrub
// release restores, the case that gets sound, and the case that does NOT become
// a shuttle run. That is not a coincidence to be tidied away: 1x forward is
// ordinary playback on the validated audio-mastered path, and the shuttle
// deliberately never enters it.
//
// NO SHUTTLE PRESS LANDS THE PREVIOUS RUN. Settled at spec phase 4; J and L
// used to disagree, J passing false and L true, and the buttons are a third and
// fourth caller that had to pass something.
//
// Two halves of the recorded justification turned out not to hold.
//
// "L must pass true or the old run's LEASE AND QUEUE would strand" is not about
// this flag at all: endShuttleRun() reclaims the lease and clears the queue
// ABOVE its landExactly branch, and startShuttle calls it unconditionally. The
// lease comes back for every value of the flag. What the flag controls is only
// the decode that follows.
//
// "J passes false because a forward run supersedes the picture immediately"
// described a mechanism that dd21fe9 removed. Before it, an off-speed forward
// run presented one frame per tick synchronously, so its picture really was
// already exact; it is a queued, strided run now, the same shape as reverse.
//
// What was left was anchoring -- the landing is a synchronous Step decode, and
// the forward run that follows decodes forward from wherever the decoder is --
// and that is a question about the decoder rather than about the picture, so it
// was measured (scripts/measure/shuttleland.ps1, reverse then L, land on vs off):
//
//   4K H.264   -1x -> +2x   land 0.8ms   48 frames both, starve 0 both, 100.2 / 100.1%
//   4K H.264   -1x -> +1x   land 0.7ms   47 frames both, handler>budget 1 (max 105.1 / 105.8)
//   1080p 412f -10x -> +2x  land 0.3ms   48 frames both, starve 0 both, 100.8 / 100.0%
//   ProRes4444 -1x -> +2x   land 25.2ms  46 vs 45 frames, starve 4 vs 5, 92.8 / 90.8%
//
// It buys nothing, and the mechanism says why. On a long-GOP file the frame is
// a REVERSE-CACHE HIT by construction -- the reverse run decoded and presented
// it moments earlier -- so the landing costs under a millisecond AND, because a
// cache hit sets the decoder's currentFrame_ but never its lastDecodedFrame, it
// does not move the decoder either. There is no anchor to buy. The -1x -> +1x
// row is the proof: ordinary 1x playback is the one path that decodes on the UI
// thread, and its first tick pays a ~105ms walk out of the reverse position
// WITH the landing exactly as it does without. On ProRes 4444 the cache cannot
// help (`rev-hit 0.0%` -- every frame is a keyframe, so nothing is ever walked
// past and cached), the landing becomes a real 25.2ms block on the UI thread,
// and it still buys one frame's difference over a two-second run.
//
// So the landing belongs to a STOP, where the standing rule applies directly:
// fidelity is owed to the frame the user stops on. K, Space and running off the
// end of the media all still pass true. A press that starts another run does
// not, because the frame it would re-decode is replaced within one frame period.
void MainWindow::startShuttle(int direction, trace::core::ShuttleEntry entry) {
    if (!frameSource_ || !frameSource_->canPlay()) return;

    // 1. End the run in progress. Every press is a new run: a new speed or a new
    //    direction invalidates everything already produced, and the generation
    //    bump inside reclaimDecoder() makes that true by construction rather
    //    than by the order two callbacks happen to run in.
    endShuttleRun(/*landExactly=*/false);

    // 2. The controller owns the ladder, and this is the only thing that picks a
    //    speed. `entry` decides the first rung and nothing else.
    if (direction < 0) playback_.jogReverse(entry);
    else               playback_.jogForward(entry);

    // The stride IS the commanded speed. It is an input the user chose, so
    // nothing the decoder measures can move it -- which is what stops it running
    // away the way three of the four scrub-gate inferences did.
    const int stride = static_cast<int>(std::lround(std::abs(playback_.state().speed)));
    const bool ordinaryForwardPlay = direction > 0 && stride <= 1;

    // 3. Intent, not state. A 1x forward run is worth restoring after a drag;
    //    an off-speed or reverse run is a different gesture and resuming it at
    //    1x would be the wrong answer.
    userPlayIntent_ = ordinaryForwardPlay;

    // 4. The decoder request. clearQueue is true for both directions now; J
    //    passed true and L passed false, and the difference was inert --
    //    clearForwardQueue() only zeroes perfStats_.forwardQueueDepth, a counter
    //    for the synchronous forward-fill queue that was removed in July 2026
    //    and is never written any other value.
    prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, direction, true);

    // 5. Audio. ONE call covers all four cases, because startAudioForPlayback()
    //    asks audioShouldDrive() first and that already means "forward, at 1x,
    //    on a video file with a track" -- so it stops the device for reverse and
    //    for every rung above 1x rather than starting it. J's separate
    //    stopAudio() was the same thing said twice.
    startAudioForPlayback();

    // 6. The timeline, NOT a bare playTimer_.start(). See section 29.2 above;
    //    this is the step whose omission is invisible until playback has been
    //    running for several seconds.
    beginPlaybackTimeline();

    // 7. Above 1x, and in reverse at any speed, the run is a shuttle: the speed
    //    is a sampling stride and presentation stays at one frame per source
    //    period. Carrying the speed in the tick rate instead caps achieved speed
    //    at per-frame decode cost, which is how ProRes 4444 asked for 2x and
    //    delivered 1.00x while 4x delivered 1.33x.
    if (!ordinaryForwardPlay) startShuttleRun(direction, stride);

    // 8. The spec's temporary rate indicator, for every surface at once because
    //    this is the one place a shuttle rate is ever chosen. Shown only for a
    //    run: ordinary 1x forward playback has no rate to announce, and the
    //    predicate that decides whether there is a run is the same one.
    flashRate(ordinaryForwardPlay
        ? QString()
        : QStringLiteral("%1%2x").arg(direction < 0 ? "-" : "+").arg(stride));
}

// One rate string, two surfaces. It used to live inside the transport bar's own
// label, which was fine while the bar was the transport; at spec phase 6 the
// floating overlay has to show the same thing, and the overlay's rateText hook
// was reading `playback_.state().speed` directly -- a SECOND source, and one
// that was permanently visible, so the overlay read "1.0x" and "PAUSED" forever
// where the spec asks for a rate that appears on a press and clears itself.
// A readout that is always on is a HUD; a transport announces a change.
void MainWindow::flashRate(const QString& text) {
    rateFlashText_ = text;
    if (transportBar_) transportBar_->flashRate(text);
    if (text.isEmpty()) rateFlashTimer_.stop();
    else rateFlashTimer_.start();
    // The overlay has no widget to invalidate itself, so the repaint is asked
    // for here -- both when the text appears and, through the timer, when it
    // goes. Without the second one the rate would linger until the next frame.
    if (viewer_) viewer_->update();
}

// The single exact-frame-step command. Left/Right reach it through
// prevFrameAction_ / nextFrameAction_, so there is one definition of what
// stepping a frame means rather than the two near-copies that were here before
// spec phase 3. Both transport buttons reached it too until phases 4 and 5 made
// them shuttle controls; the actions are unchanged, only their surfaces are.
//
// The copies had diverged, and one difference was a real bug: the BUTTON path
// never called endShuttleRun(), so clicking a frame-step button during a reverse
// run left shuttleRunActive_ true and shuttleLastPresented_ holding the
// SHUTTLE's frame. The step itself looked fine -- the run cannot present
// anything once stepBackward() has paused the controller and the timer is
// stopped -- so the fault is invisible until the NEXT thing that ends a run
// takes its landing branch. Press K after that click and
// setCurrentFrame(shuttleLastPresented_) puts the playhead back where the
// shuttle was, discarding the frame the user stepped to.
//
// MEASURED, on a control built from cbf6d98 against this change, 4K H.264:
// reverse, click Prev Frame, settle, then K. Control moves the picture by
// **17.6%** on the K press; after this change it moves **0%**. The gesture
// preceding it -- click, then step with the arrow key -- passes identically on
// both, which is why the hole survived: it does not hang and it does not freeze.
//
// revtransitions.ps1 enumerates six ways out of a reverse run and every one is a
// key or the slider. The buttons are a seventh, and nothing exercised them.
//
// The other difference between the two copies was inert: Left passed
// clearQueue=true and Right false, and clearForwardQueue() only zeroes
// perfStats_.forwardQueueDepth, a counter for the synchronous forward-fill queue
// removed in July 2026 that is never written any other value.
void MainWindow::stepOneFrame(int delta, const char* hudLabel) {
    // End a shuttle run WITHOUT a landing decode: the step below lands its own
    // frame, and the playhead is already the frame that was on screen, because
    // every present sets it from the frame's own index.
    endShuttleRun(/*landExactly=*/false);

    if (delta < 0) playback_.stepBackward();
    else           playback_.stepForward();

    prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step,
                        delta < 0 ? -1 : 1, true);

    // Outside any isActive() guard: stepping is a request to inspect a frame
    // whether or not playback happened to be running, and the intent must not
    // survive it into a later scrub release.
    userPlayIntent_ = false;
    if (playTimer_.isActive()) {
        playTimer_.stop();
        stopAudio();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;
    }

    // A step is a landing: one frame, exact, full resolution. On long-GOP media
    // a BACKWARD step that leaves the walked run pays a seek and a fresh GOP
    // walk, and on the single-keyframe Seedance clip that walk starts at frame
    // 0 -- measured as `2 3 2 4 2 4 2 2 2 411 2 2 3`, one step in thirteen
    // freezing the window for 411ms. Off the UI thread it is the same 411ms of
    // decoding with the window alive through it.
    //
    // Rapid presses now COALESCE, and that is a real behaviour change worth
    // stating. Each press advances the playhead and supersedes the landing in
    // flight, so five fast presses move five frames and decode the fifth. Every
    // frame in between used to be decoded and drawn, serially, which is why
    // holding an arrow key on heavy media felt like wading. The contract is
    // unharmed: the arithmetic is identical, +5 then -5 returns to the same
    // frame, and the frame the user stops on is exact. This is latest-target-
    // wins applied to a gesture that always had it in the pointer path.
    if (requestExactFrameAsync(playback_.state().currentFrame,
                               delta < 0 ? -1 : 1, LandingKind::Step)) {
        landingStepDelta_ = delta;
        landingHudLabel_ = hudLabel;
        refreshHud(hudLabel);
        return;
    }

    ++landingsSync_;
    QString error;
    if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
        // Put the playhead back where it was AND re-land the frame it names: a
        // step that could not be decoded must not leave the counter claiming a
        // frame the viewer is not showing. The button path did this and the
        // keyboard path only reverted the counter; the button's is the correct
        // half and both take it now.
        if (delta < 0) playback_.stepForward();
        else           playback_.stepBackward();
        loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step);
    } else if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
        prefetchNeighbors();
    }
    refreshHud(hudLabel);
}

// Everything a run needs that is not the decoder request and not the audio.
//
// J and L used to start playTimer_ themselves and set only playbackClock_, so
// none of the rest of this ran on their path. sessionClock_ is the one that
// turned a cosmetic omission into a playback fault: syncPresentTimeline and
// armNextPresent both guard on isValid() and fall through to `: 0`, which makes
// `target = 0 + slot * period` permanently greater than `now == 0`. The rephase
// branch that exists to catch a schedule running ahead can then never fire, and
// the armed delay grows by one frame period per tick -- 792ms by the 19th, which
// is 8 seconds in. Measured before the fix: 20 presents in 8s on J, against 111
// on the same file with TRACE_DEADLINE_SCHED=0. See plan section 29.2.
//
// Called on every J and L press, not only the first. That is deliberate: each
// press is a new speed or direction, so it is a new run, and its cadence figures
// should be measured from the press rather than averaged across the one before.
void MainWindow::beginPlaybackTimeline() {
    playbackClock_.start();
    playbackAccumulatorMs_ = 0.0;
    // Presented-rate window starts with the play action, so pausing and
    // resuming measures the new run rather than averaging across the gap.
    playbackRateClock_.start();
    sessionClock_.start();
    firstPresentNs_ = -1;
    lastPresentNs_ = -1;
    playbackFramesPresented_ = 0;
    playbackRunElapsedS_ = 0.0;
    frameCycleClock_.invalidate();
    cycleSamples_ = 0;
    lastHandlerMs_ = avgHandlerMs_ = 0.0;
    lastPeriodMs_ = avgPeriodMs_ = maxPeriodMs_ = 0.0;
    lastOutsideMs_ = avgOutsideMs_ = 0.0;
    schedulerTickClock_.invalidate();
    schedulerTicks_ = 0;
    presentSamples_ = 0;
    lastTickJitterMs_ = avgTickJitterMs_ = maxTickJitterMs_ = 0.0;
    lastPresentLatencyMs_ = avgPresentLatencyMs_ = maxPresentLatencyMs_ = 0.0;
    lastDriftMs_ = 0.0;
    lastAvSyncMs_ = maxAvSyncMs_ = 0.0;
    audioRepeatedFrames_ = audioSkippedFrames_ = 0;
    playbackDroppedFrames_ = playbackDropTicks_ = maxDropRun_ = 0;

    lastClockUpdateMark_ = -1;
    lastClockUpdatesPerTick_ = maxClockUpdatesPerTick_ = 0;
    cadenceGapsMs_.clear();
    cadenceLongAt_.clear();
    handlerSamples_ = handlerOverBudget_ = 0;
    maxHandlerMs_ = 0.0;
    // GATE E: invalidate the presentation timeline rather than establishing it
    // here. The first tick sets the epoch, because the period depends on the
    // playback speed and that is read inside the tick.
    presentEpochNs_ = -1;
    presentTargetNs_ = -1;
    presentSlot_ = 0;
    presentPeriodNs_ = 0.0;
    presentSlotLatencyMs_ = 0.0;
    presentRephaseCount_ = 0;
    playTimer_.start();
}

// ---- Reverse shuttle --------------------------------------------------------
//
// Reverse runs decode on the SAME worker and under the SAME lease as the drag.
// The one thing that differs is that the target is arithmetic -- at stride S the
// next frame wanted is always `lastAsked - S` -- so the worker can be run ahead
// without speculating. That is why the lookahead declined for the drag path at
// plan section 15.3 is the right answer here and the wrong one there, and it is
// a re-derivation rather than a reversal: the drag's worker was measured
// saturated at 59-74% supply, and reverse at 1x measures 80-93% IDLE.
//
// Results are queued rather than presented on arrival, and the playback tick
// pops one per slot. That is the cadence fix in one sentence: a ~130ms GOP walk
// is absorbed by the queue instead of landing inside a 41.67ms slot.

// How many decoded frames to keep ahead of the presentation point.
//
// Sized from the measured worst case rather than guessed: the longest reverse
// handler on 4K H.264 is ~132ms against a 41.67ms slot, so three frames covers
// it at 1x and twelve at 4x. Eight is the compromise, and it is frames rather
// than milliseconds because a frame is what the tick consumes. Memory is a
// reference count, not a copy -- these buffers are reverse-cache entries.
constexpr int kShuttleQueueDepth = 8;

// TRACE_REVERSE_ASYNC=0 keeps reverse on the synchronous UI-thread path. Its own
// knob rather than sharing TRACE_ASYNC_SCRUB, so a reverse A/B does not also
// change how dragging behaves -- the same reason TRACE_GPU_REDUCE is separate
// from TRACE_PLANAR_UPLOAD.
static bool shuttleAsyncEnabled() {
    static const bool on = [] { return qgetenv("TRACE_REVERSE_ASYNC") != "0"; }();
    return on;
}

void MainWindow::startShuttleRun(int direction, int stride) {
    if (!shuttleAsyncEnabled() || !asyncScrubEnabled()) return;
    if (!currentMedia_.has_value() || currentMedia_->kind != MediaKind::VideoFile) return;
    // A drag owns the decoder while it is happening; the shuttle waits its turn
    // rather than fighting for the lease.
    //
    // `scrubbing_`, NOT isVideoScrubActive(): the latter reads like "a scrub is
    // active" and means "the media is a video file", so it is true for every
    // case this function exists to serve. Guarding on it silently disabled the
    // whole pipeline while every other counter looked healthy -- `posted 0` on
    // the worker line was the only visible symptom.
    if (scrubbing_ || storageBusy_) return;

    // Every press is a new run: a new speed or a new direction invalidates
    // everything already produced, and the generation bump inside
    // reclaimDecoder() is what makes that true by construction rather than by
    // the order two callbacks happen to run in.
    endShuttleRun(/*landExactly=*/false);

    shuttleDir_ = direction < 0 ? -1 : 1;
    shuttleStride_ = std::max(1, stride);
    // Starts unsnapped whatever the last run did; pumpShuttleQueue decides per
    // request once the grid is known.
    shuttleSnapping_ = false;
    shuttleAdvance_ = shuttleStride_;
    shuttleLastPresented_ = playback_.state().currentFrame;
    shuttleNextTarget_ = shuttleLastPresented_ + shuttleDir_ * shuttleStride_;
    if (!shuttleTargetInRange(shuttleNextTarget_)) shuttleNextTarget_ = -1;
    shuttleRunActive_ = true;
    shuttleStarves_ = 0;
    shuttleQueueMaxSeen_ = 0;
    pumpShuttleQueue();
}

// Snap only backward, only on long-GOP, and only once the speed leaves room for
// at most two presented frames per GOP. Below that the walk is amortised over
// several presented frames and is worth paying; above it the walk is pure waste.
//
// Forward is deliberately excluded and measures fine without it: a forward step
// of `stride` walks forward from where the decoder already is, which costs
// ~0.9-2.6ms a frame and no seek at all. Only backward pays the intercept.
bool MainWindow::shuttleShouldSnap() const {
    if (shuttleDir_ > 0) return false;
    if (videoDecoder_.metadata().intraOnly) return false;  // no GOP to snap to
    if (shuttleGop_ <= 1) return false;                    // not learned yet
    return shuttleStride_ * 2 >= shuttleGop_;
}

// The keyframe at or before `ideal`, on the grid anchored at an observed one.
long long MainWindow::shuttleSnapTarget(long long ideal) const {
    if (shuttleGop_ <= 1 || ideal < 0) return ideal;
    const long long base = shuttleKfAnchor_ >= 0 ? shuttleKfAnchor_ : 0;
    // Steps from the anchor down to at-or-below `ideal`. Anchored rather than
    // taken modulo the spacing, because a file whose first keyframe is not at
    // frame 0 has a grid that multiples of the spacing simply miss.
    const long long delta = base - ideal;
    if (delta <= 0) return ideal < base ? ideal : base;
    const long long steps = (delta + shuttleGop_ - 1) / shuttleGop_;
    const long long snapped = base - steps * shuttleGop_;
    return snapped >= 0 ? snapped : 0;
}

// Is a target still inside the media? The head and the tail are different
// expressions, which is the whole reason this is a function rather than a
// `< 0` test copied to three call sites.
bool MainWindow::shuttleTargetInRange(long long frame) const {
    if (frame < 0) return false;
    const long long maxFrame = playback_.state().maxFrame;
    return maxFrame < 0 || frame <= maxFrame;
}

void MainWindow::endShuttleRun(bool landExactly) {
    if (!shuttleRunActive_) {
        // Still safe to call: an inactive run has nothing to reclaim and the
        // queue is already empty. Callers do not have to know which.
        shuttleQueue_.clear();
        return;
    }
    shuttleRunActive_ = false;
    // Bumps the generation and waits for the worker to park, so nothing it
    // produced during the wait can ever be presented afterwards. Same single
    // choke point the drag release goes through.
    reclaimDecoder();
    shuttleQueue_.clear();
    shuttleNextTarget_ = -1;

    if (landExactly && shuttleLastPresented_ >= 0) {
        QElapsedTimer landClock;
        landClock.start();
        // Land on the frame that was ON SCREEN, never on the one the arithmetic
        // had run ahead to. The queue may hold frames the user never saw, and
        // stopping on one of those would move the picture after the user asked
        // it to stop. Owner-confirmed as the correct behaviour, 2026-08-10.
        playback_.setCurrentFrame(shuttleLastPresented_);
        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step,
                            shuttleDir_, true);
        QString error;
        loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step);
        shuttleLandLastMs_ = static_cast<double>(landClock.nsecsElapsed()) / 1'000'000.0;
        shuttleLandMaxMs_ = std::max(shuttleLandMaxMs_, shuttleLandLastMs_);
        ++shuttleLandCount_;
    }
    shuttleLastPresented_ = -1;
}

void MainWindow::pumpShuttleQueue() {
    if (!shuttleRunActive_) return;
    if (shuttleNextTarget_ < 0) return;
    // One request in flight at a time -- the chain re-posts from onScrubResult,
    // exactly as the drag does. Depth comes from the queue, not from stacking
    // requests at the worker, so `latest wins` keeps meaning one thing.
    if (scrubWorker_.busy()) return;
    if (static_cast<int>(shuttleQueue_.size()) >= kShuttleQueueDepth) return;
    if (storageBusy_) return;

    // Re-decided per request, because the GOP is learned as the run goes: the
    // first request or two run unsnapped, and snapping engages as soon as the
    // grid is known. A 30x run is short, so paying one unsnapped request for a
    // measured grid is cheaper than probing for one at open.
    const bool snap = shuttleShouldSnap();
    // Recomputed every request, not only on the transition. The first cut set it
    // once when snapping engaged and then let the learned grid move underneath
    // it, so the pacing was computed from a spacing that no longer applied --
    // it read `adv 20` on a 48-frame grid.
    const long long advance = snap ? shuttleGop_ : shuttleStride_;
    if (snap != shuttleSnapping_ || advance != shuttleAdvance_) {
        shuttleSnapping_ = snap;
        // The presentation period changes with it. syncPresentTimeline re-epochs
        // on a period change, which is exactly what a mode change needs.
        shuttleAdvance_ = advance;
    }
    if (snap) {
        const long long snapped = shuttleSnapTarget(shuttleNextTarget_);
        if (snapped >= 0) shuttleNextTarget_ = snapped;
    }

    grantDecoderLease();
    trace::core::ScrubRequest request;
    request.frame = shuttleNextTarget_;
    request.direction = shuttleDir_;
    request.generation = requestGeneration_;
    // Playback, not Scrub: a drag preview is deliberately reduced-resolution
    // above 1920px and a shuttle run is playback, so the frame on screen has to
    // be the frame rather than a preview of one.
    request.mode = trace::core::VideoDecoderFFmpeg::RequestMode::Playback;
    scrubInFlightDir_ = shuttleDir_;
    const long long step = shuttleSnapping_ ? shuttleGop_ : shuttleStride_;
    const long long next = shuttleNextTarget_ + shuttleDir_ * step;
    shuttleNextTarget_ = shuttleTargetInRange(next) ? next : -1;
    scrubWorker_.post(request);
}

long long MainWindow::playbackQueueBytes() const {
    long long bytes = 0;
    for (const auto& e : playbackQueue_) {
        if (e.frame.buffer) bytes += static_cast<long long>(e.frame.sizeInBytes());
    }
    return bytes;
}

// Depth from the byte budget and the count cap, with the entry size taken from
// a frame that actually exists rather than predicted from the container.
//
// Predicting it is the mistake plan section 11a records in live code:
// reverseCacheCapacity is still `384MB / (w*h*4)`, the BGRA footprint, so since
// GATE C it reads 11 at 4K where planar entries really give 32 -- and it
// silently clamped an experiment to nothing while looking like a refuted
// hypothesis. Measuring one entry cannot drift that way.
int MainWindow::playbackQueueDepthForMedia() const {
    const int requested = playbackQueueRequestedDepth();
    if (requested <= 0) return 0;
    long long entryBytes = 0;
    if (!playbackQueue_.empty() && playbackQueue_.front().frame.buffer) {
        entryBytes = static_cast<long long>(playbackQueue_.front().frame.sizeInBytes());
    } else if (videoFrameBuffer_.buffer) {
        entryBytes = static_cast<long long>(videoFrameBuffer_.sizeInBytes());
    }
    if (entryBytes <= 0) return requested;
    const long long budget =
        static_cast<long long>(playbackQueueBudgetMb()) * 1024LL * 1024LL;
    const long long fits = std::max<long long>(1, budget / entryBytes);
    return static_cast<int>(std::min<long long>(requested, fits));
}

void MainWindow::startPlaybackPrefetch() {
    if (playbackQueueRequestedDepth() <= 0) return;
    if (!asyncScrubEnabled()) return;
    // Video files only, and nobody else may be holding the decoder. These
    // mirror startShuttleRun's guards, including that the drag test is
    // `scrubbing_` and NOT isVideoScrubActive() -- that one means "the media is
    // a video file" and guarding on it disabled the whole reverse pipeline once
    // while every other counter looked healthy.
    if (!currentMedia_.has_value() || currentMedia_->kind != MediaKind::VideoFile) return;
    if (shuttleRunActive_ || scrubbing_ || storageBusy_) return;
    if (landingPending_) return;

    playbackQueue_.clear();
    playbackPrefetchExhausted_ = false;
    playbackPrefetchActive_ = true;
    // The frame AFTER the one on screen. The tick's first target under ordinary
    // playback is current+1, and if it is not -- an audio catch-up on the very
    // first slot -- the consume path re-seeds rather than presenting the wrong
    // frame.
    playbackPrefetchNext_ = playback_.state().currentFrame + 1;
    pumpPlaybackQueue();
}

void MainWindow::stopPlaybackPrefetch() {
    playbackQueue_.clear();
    playbackPrefetchActive_ = false;
    playbackPrefetchNext_ = -1;
    playbackPrefetchExhausted_ = false;
    pqBytes_ = 0;
}

void MainWindow::pumpPlaybackQueue() {
    if (!playbackPrefetchActive_) return;
    if (playbackPrefetchNext_ < 0 || playbackPrefetchExhausted_) return;
    // One request in flight at a time. Depth comes from the QUEUE, not from
    // stacking requests at the worker, so the worker's depth-1 latest-wins
    // contract keeps meaning one thing for all three of its callers.
    if (scrubWorker_.busy()) return;
    if (static_cast<int>(playbackQueue_.size()) >= playbackQueueDepthForMedia()) return;
    if (storageBusy_) return;
    const long long maxFrame = playback_.state().maxFrame;
    if (maxFrame >= 0 && playbackPrefetchNext_ > maxFrame) {
        playbackPrefetchNext_ = -1;
        return;
    }

    grantDecoderLease();
    trace::core::ScrubRequest request;
    request.frame = playbackPrefetchNext_;
    request.direction = 1;
    request.generation = requestGeneration_;
    // Playback, not Scrub. A drag preview is deliberately reduced-resolution
    // above 1920px, and this frame is going on screen as the picture rather
    // than as a preview of it.
    request.mode = trace::core::VideoDecoderFFmpeg::RequestMode::Playback;
    // One frame per request, always. The scrub batch exists to amortise a
    // cross-thread round trip against a cheap frame during a drag; here the
    // queue itself is what hides the round trip, and a batch would only make
    // the depth bound harder to reason about.
    request.batch = 1;
    request.batchBudgetMs = 0.0;
    scrubInFlightDir_ = 1;
    ++playbackPrefetchNext_;
    ++pqPosted_;
    scrubWorker_.post(request);
}

bool MainWindow::presentQueuedPlaybackFrame(long long targetFrame) {
    QElapsedTimer waitClock;
    waitClock.start();

    // THE TARGET HAS MOVED PAST FRAMES WE ARE HOLDING. An audio catch-up
    // advances up to 3, and the real-time drop advances by whatever the run is
    // behind, so entries below the target are frames nobody is going to ask for
    // again. Discard them and count it -- this is the queue obeying the clock,
    // not the queue being wrong.
    while (!playbackQueue_.empty()
           && playbackQueue_.front().frame.frameIndex < targetFrame) {
        playbackQueue_.pop_front();
        ++pqAheadDrops_;
    }

    const bool haveTarget = !playbackQueue_.empty()
        && playbackQueue_.front().frame.frameIndex == targetFrame;

    if (!haveTarget) {
        // Either the queue is empty (a genuine starve) or its head is AHEAD of
        // the target. The second is the defensive branch the design names: an
        // audio hold returns before reaching here, so a head in the future means
        // the target jumped backward, and nothing held answers it.
        //
        // Re-seed whenever the lookahead position no longer tracks the target,
        // or the run would starve for ever: the position only ever advances,
        // so a target that jumped clear of it is never reached by waiting.
        const bool headAhead = !playbackQueue_.empty()
            && playbackQueue_.front().frame.frameIndex > targetFrame;
        if (headAhead || playbackPrefetchNext_ < targetFrame) {
            playbackQueue_.clear();
            playbackPrefetchNext_ = targetFrame;
            playbackPrefetchExhausted_ = false;
            ++pqReseeds_;
        }
        pumpPlaybackQueue();
        pqWaitMaxMs_ = std::max(
            pqWaitMaxMs_,
            static_cast<double>(waitClock.nsecsElapsed()) / 1'000'000.0);
        return false;
    }

    // STOPPED HERE, BEFORE THE PRESENT. `wait` is time the tick spent waiting on
    // the QUEUE, and everything below is the present itself -- setFrame() is
    // where the D3D11 upload happens, 24.58ms of it on an 8K plate. Timing to
    // the end of the function folded that in and the field read `wait 52.01ms`
    // on a run where nothing had waited for anything: it was reporting the
    // upload under a name that means "the pipeline blocked me". A field that
    // must read 0 has to be measured over only the thing that could make it
    // non-zero.
    pqWaitMaxMs_ = std::max(
        pqWaitMaxMs_,
        static_cast<double>(waitClock.nsecsElapsed()) / 1'000'000.0);

    const PlaybackFrame queued = playbackQueue_.front();
    playbackQueue_.pop_front();
    videoFrameBuffer_ = queued.frame;
    // Identity off the frame, never off the arithmetic that asked for it --
    // presentQueuedShuttleFrame's rule, for the e76eabb reason. A frame that
    // landed off-target is visibly off-target in target/shown/delta rather than
    // silently relabelled.
    lastRequestedFrame_ = queued.requested;
    lastDeliveredFrame_ = videoFrameBuffer_.frameIndex;
    playback_.setCurrentFrame(videoFrameBuffer_.frameIndex);
    viewer_->setFrame(videoFrameBuffer_);
    // update(), not repaint(): one frame per slot, so there is no chain of
    // paints to coalesce, and blocking on the paint here would put it inside
    // the handler measurement this change is judged by.
    viewer_->update();

    // Refill behind the frame just consumed.
    pumpPlaybackQueue();
    pqMaxDepth_ = std::max<long long>(pqMaxDepth_,
                                      static_cast<long long>(playbackQueue_.size()));
    pqBytes_ = playbackQueueBytes();
    pqPeakBytes_ = std::max(pqPeakBytes_, pqBytes_);
    return true;
}

bool MainWindow::presentQueuedShuttleFrame() {
    if (shuttleQueue_.empty()) return false;
    const ShuttleFrame queued = shuttleQueue_.front();
    shuttleQueue_.pop_front();
    videoFrameBuffer_ = queued.frame;
    // Identity comes off the frame itself, never off the arithmetic that asked
    // for it. A frame that landed off-target is then visibly off-target in
    // `target`/`shown`/`delta` rather than silently relabelled -- which is the
    // e76eabb failure and the July 2026 scrub failure, both at once.
    lastRequestedFrame_ = queued.requested;
    lastDeliveredFrame_ = videoFrameBuffer_.frameIndex;
    shuttleLastPresented_ = videoFrameBuffer_.frameIndex;
    playback_.setCurrentFrame(videoFrameBuffer_.frameIndex);
    viewer_->setFrame(videoFrameBuffer_);
    // update(), not repaint(): the tick presents exactly one frame per slot, so
    // there is no chain of paints to coalesce and blocking on the paint here
    // would put it inside the handler measurement.
    viewer_->update();
    // Refill behind the frame just consumed.
    pumpShuttleQueue();
    return true;
}

// Present accounting for one presented frame. Extracted from the playback tick
// so forward playback and the reverse shuttle are measured by ONE instrument
// rather than by two that have to be kept in agreement -- the same reason
// beginPlaybackTimeline() was extracted at section 29.3, and the same failure it
// avoids: a second copy that silently stops matching.
void MainWindow::notePresentedPlaybackFrame(double frameDurationMs) {
    ++playbackFramesPresented_;
    playbackRunElapsedS_ = static_cast<double>(playbackRateClock_.elapsed()) / 1000.0;
    // First/last present span: N presented frames cover N-1 intervals, so rate
    // from this span is the honest steady-state figure.
    const qint64 nowNs = sessionClock_.isValid() ? sessionClock_.nsecsElapsed() : 0;
    if (firstPresentNs_ < 0) firstPresentNs_ = nowNs;

    // Cadence distribution, not just its mean. The presented rate reads 98-99%
    // under two completely different faults and therefore cannot tell them
    // apart, which is why a file can measure 99% and still visibly stutter:
    //
    //   the integer tick beat -- floor(1000/fps) is 41ms against a 41.667ms
    //   frame, so the accumulator falls 0.667ms short each frame and roughly
    //   every 62nd frame needs two ticks. That is one doubled (~83ms) frame
    //   every ~2.6s, REGULARLY spaced, on every file. GATE E owns it.
    //
    //   per-frame cost overrun -- a frame that misses the budget is late at once
    //   and nothing absorbs it. Ragged, irregular, and specific to whichever
    //   file is expensive. A presentation clock supplies phase, not headroom, so
    //   GATE E does NOT fix this one.
    //
    // The spacing between long frames is what separates them: a beat is regular,
    // an overrun is not. Sampling intervals BETWEEN PRESENTS rather than between
    // ticks is deliberate -- a held frame produces no present, so the doubled
    // interval only appears here.
    if (lastPresentNs_ > 0) {
        const double gapMs = static_cast<double>(nowNs - lastPresentNs_) / 1'000'000.0;
        if (cadenceGapsMs_.size() < kCadenceSampleCap) {
            cadenceGapsMs_.push_back(gapMs);
            if (frameDurationMs > 0.0 && gapMs > frameDurationMs * 1.5) {
                cadenceLongAt_.push_back(playbackFramesPresented_);
            }
        }
    }
    lastPresentNs_ = nowNs;
    // Clock drift: ideal media time for the frames presented so far versus wall
    // clock. Positive = ahead of real time, negative = behind.
    lastDriftMs_ = static_cast<double>(playbackFramesPresented_) * frameDurationMs
                 - playbackRunElapsedS_ * 1000.0;
}

// How many frames of media time this present should advance, so that a source
// which cannot sustain its native rate holds real time and drops picture instead
// of playing the whole movie slowly (owner decision, 2026-08-13).
//
// IT RETURNS 1 UNLESS THE RUN IS ALREADY BEHIND, so a source that keeps up is on
// exactly the path it was on before and `drop` reads 0 through every run in the
// validated asset set. That is the "engage only when required" requirement made
// structural rather than tuned: there is no threshold to pick.
//
// CLOCK-DRIVEN, and the clock is the GATE E presentation timeline rather than the
// wall-clock accumulator. The accumulator saturates -- it is capped at four
// periods so a stalled run resumes at rate rather than fast-forwarding through
// arrears -- so `floor(accumulator / period)` pins at 4 on a source running at
// half rate and would ask for four frames of media per present, i.e. nearly
// double speed. `presentAnchorFrame_ + elapsed/period` is the frame that ought to
// be on screen now, does not saturate, and cannot drift.
//
// THE ONE SANCTIONED CASE IS 1x FORWARD PLAYBACK AND NOTHING ELSE. Reverse and
// the shuttle carry their speed in a stride and return before this is reached;
// stepping, scrub, the scrub release and any paused frame never come through this
// path at all. Exactness there is unchanged and is not negotiable.
//
// The cap is the accumulator's own backlog policy, in frames: a long stall -- a
// resize, a hitch, a window drag -- must resume at rate rather than fast-forward
// through everything it missed, and four frames of media time per present is
// already 4x real time on a source that is merely late rather than slow.
long long MainWindow::realtimeDropSteps(int direction,
                                        const trace::core::PlaybackState& playbackState,
                                        double frameDurationMs) {
    constexpr long long kMaxDropAdvance = 4;
    static const bool dropEnabled = qgetenv("TRACE_RT_DROP") != "0";

    if (!dropEnabled) return 1;
    if (direction <= 0) return 1;
    if (playbackState.mode != trace::core::PlaybackMode::PlayingForward) return 1;
    // Below 1x the user has asked for slow motion, and dropping picture to hold a
    // deliberately slowed clock would defeat the request. 0.5x therefore presents
    // every frame however heavy the source -- the same reasoning that makes it
    // silent rather than resampled.
    if (playbackState.speed < 1.0) return 1;
    if (presentEpochNs_ < 0 || presentPeriodNs_ <= 0.0) return 1;
    if (!sessionClock_.isValid() || frameDurationMs <= 0.0) return 1;

    const qint64 now = sessionClock_.nsecsElapsed();
    const long long dueFrame = presentAnchorFrame_
        + static_cast<long long>(std::floor(static_cast<double>(now - presentEpochNs_)
                                            / presentPeriodNs_));

    // `+ 1` because presenting the next frame is what being on time looks like.
    const long long behind = dueFrame - (playbackState.currentFrame + 1);
    if (behind <= 0) return 1;

    const long long steps = std::min(behind + 1, kMaxDropAdvance);
    const long long dropped = steps - 1;
    playbackDroppedFrames_ += dropped;
    ++playbackDropTicks_;
    maxDropRun_ = std::max(maxDropRun_, dropped);
    return steps;
}

// Tell the decoder whether playback has proved itself SEQUENTIAL, which is what
// lets it take frame threading. The hysteresis is deliberately one-sided, and the
// direction is the whole design.
//
// SLICE IS THE STARTING STATE AND A HEAVY SOURCE NEVER LEAVES IT. Frame threading
// is earned by twelve consecutive presents that dropped nothing, and lost on the
// first one that drops. The reason is that the switch is a codec reopen, and on
// intra-only at 8K a frame-threaded `avcodec_open2` builds thread_count contexts
// over a 7680x4320 frame -- measured at up to ~850ms, which is 20 frame periods.
//
// The first cut had it the other way round: start frame-threaded, switch out
// after three drops. Steady state was identical (`p50 89.5ms` either way) but the
// run paid two reopens on entry and read `max 892.9ms`, so a 5s measurement lost
// half its span to a transient. Starting in the safe mode means the source that
// cannot keep up pays NOTHING to discover that -- `sw 0` -- while the source that
// can afford frame threading is by definition the one whose reopen is cheap and
// which has half a second of clean presents to spare.
//
// GATE E step 1. Establishes the presentation timeline, and re-establishes it
// when the period changes under it.
//
// Idempotent by design: it is called from every tick and does nothing on the
// overwhelming majority of them. A speed change is the one thing that has to
// re-epoch, because a new period measured from an old epoch is a schedule with
// a step discontinuity in it -- the next deadline would land wherever the two
// timelines happened to cross.
void MainWindow::syncPresentTimeline(double frameDurationMs) {
    if (!deadlineScheduleEnabled()) return;
    const double periodNs = frameDurationMs * 1'000'000.0;
    if (periodNs <= 0.0) return;

    // 1us of tolerance. The period is recomputed from the rational every tick
    // and is bit-identical across ticks at a steady speed, so this only trips
    // on a real change; the tolerance is there so a future non-exact source
    // cannot re-epoch itself continuously.
    const bool periodChanged = std::abs(periodNs - presentPeriodNs_) > 1000.0;
    if (presentEpochNs_ >= 0 && !periodChanged) return;

    presentPeriodNs_ = periodNs;
    presentEpochNs_ = sessionClock_.isValid() ? sessionClock_.nsecsElapsed() : 0;
    presentSlot_ = 0;
    presentTargetNs_ = presentEpochNs_;
    // WHERE the timeline starts, beside when. Play from the middle of a file
    // epochs here with currentFrame in the middle, and the real-time drop maps
    // elapsed media time onto a frame index through this anchor.
    presentAnchorFrame_ = playback_.state().currentFrame;
}

// GATE E step 1. Advances the grid slot and arms playTimer_ for that slot's
// absolute deadline.
//
// Called from a scope guard on every exit path of the playback tick. The slot
// advances on EVERY wake, presented or held, so the heartbeat stays regular and
// "which frame" is left entirely to the audio clock (plan section 24.3).
void MainWindow::armNextPresent() {
    // The control path leaves playTimer_ free-running at its open-time interval,
    // which is exactly the pre-GATE-E behaviour.
    if (!deadlineScheduleEnabled()) return;

    // Every playTimer_.stop() in the codebase keeps working unchanged because
    // of this line: a stopped run is not re-armed.
    if (!playTimer_.isActive()) return;

    if (presentEpochNs_ < 0 || presentPeriodNs_ <= 0.0) {
        // No timeline yet -- the first tick has not run, or the media has no
        // usable rate. Fall back to the nominal interval; the next tick
        // establishes the timeline and takes over.
        playTimer_.setInterval(schedulerIntervalMs_);
        return;
    }

    const qint64 now = sessionClock_.isValid() ? sessionClock_.nsecsElapsed() : 0;
    ++presentSlot_;
    qint64 target = presentEpochNs_
                  + static_cast<qint64>(std::llround(static_cast<double>(presentSlot_) * presentPeriodNs_));

    if (target <= now) {
        // The handler overran its slot. Re-phase to the next slot strictly
        // after now instead of banking the debt -- this is the analogue of the
        // old 4-frame accumulator cap, and it exists for the same reason: a run
        // that stalled must resume AT RATE, not fast-forward through arrears.
        //
        // The epoch is NOT moved. Keeping it means the timeline stays anchored
        // to where the run started, so a single long frame costs one slot
        // rather than permanently shifting every deadline after it.
        // Not named `slots`: Qt defines that as a macro, and `const double
        // slots = ...` silently becomes `const double = ...`.
        const double elapsedSlots = static_cast<double>(now - presentEpochNs_) / presentPeriodNs_;
        presentSlot_ = static_cast<long long>(std::floor(elapsedSlots)) + 1;
        target = presentEpochNs_
               + static_cast<qint64>(std::llround(static_cast<double>(presentSlot_) * presentPeriodNs_));
        ++presentRephaseCount_;

        // A saturated pipeline has nothing to wait for. The slot still advances
        // and the epoch is still untouched, so the timeline is unchanged and the
        // frame index is still chosen by the accumulator or the audio clock --
        // this only declines to sit idle until the next grid slot. See
        // freerunWhenSaturated().
        if (freerunWhenSaturated() && tickFrameDurationMs_ > 0.0
            && lastHandlerMs_ > tickFrameDurationMs_) {
            target = now;
        }
    }

    presentTargetNs_ = target;
    const double delayMs = static_cast<double>(target - now) / 1'000'000.0;
    // Rounded to a millisecond because QTimer takes an int -- but the NEXT arm
    // is computed from the next absolute deadline, not from this one, so the
    // rounding error does not accumulate. At 24fps the arms alternate 41/42 and
    // average the true 41.667. That is the whole fix.
    schedulerIntervalMs_ = std::max(0, static_cast<int>(std::lround(delayMs)));
    // setInterval on an active timer restarts it, which is exactly the re-arm
    // wanted here and is why playTimer_ stays a periodic timer rather than
    // becoming single-shot: every existing isActive()/stop() call site keeps
    // its meaning.
    playTimer_.setInterval(schedulerIntervalMs_);
}

// Scrubbing suspends playback; it does not end it. The intent flag is what
// survives the drag, and this is the only place that reads it.
//
// Ordering is load-bearing and the caller has already done the first half:
// flushVideoScrub(true) landed the exact frame, and the reclaimDecoder() inside
// that landing bumped the request generation AND pushed it at the worker, so no
// older preview-resolution frame can be painted after it. Resuming before the
// landing would also start audio at the preview position, because
// startAudioForPlayback() takes its offset from the current frame.
//
// There is deliberately no supersedeInFlightRequests() call here, and adding
// one would not do the job anyway. That function bumps requestGeneration_ but
// deliberately does NOT tell the worker (see its own comment), while
// onScrubResult drops on scrubWorker_.latestGeneration() -- so a bare call
// would leave an in-flight worker result perfectly deliverable. The pairing
// that actually closes the window is scrubWorker_.supersede(...), and
// reclaimDecoder() is the one place that performs it.
void MainWindow::resumePlaybackAfterScrub() {
    if (!userPlayIntent_) return;
    if (!frameSource_ || !frameSource_->canPlay()) return;
    // The landing was deferred -- storage was mid-read, so flushVideoScrub
    // re-armed instead of decoding. Resuming now would play from a frame the
    // release has not landed yet. Staying paused is the safe half of that
    // trade; the user can press Play once the frame arrives.
    if (pendingScrubFrame_ >= 0) return;
    // Released on the last frame. Restarting the file here would be a jump the
    // user did not ask for; leave it landed. Play already handles the rewind.
    if (playbackAtEnd_) return;
    // Paused by the scrub lambdas, so this puts it back to 1x forward. Asked
    // rather than assumed: togglePlayPause() on a state that is somehow already
    // playing would pause it, which is the opposite of the job.
    if (playback_.state().mode == PlaybackMode::Paused) playback_.togglePlayPause();
    startPlaybackRun();
}

// Sound only at 1x forward. Off-speed J-K-L, reverse, scrubbing and stepping
// are deliberately silent in this build: resampled or reversed audio is a
// separate piece of work, and half-working sound is worse than none in a
// review tool.
// EXACTLY 1x, NOT "AT MOST 1x", and spec phase 14 is what made the difference
// observable. This read `<= 1.0001` from the day it was written, which was
// correct for as long as the ladder's slowest rung WAS 1x -- there was no speed
// below it, so "at most 1x" and "exactly 1x" named the same set.
//
// 0.5x is the first speed that separates them, and the failure it would have
// produced is one this project has already measured once. Under `<=` the device
// would run at real time while the picture presented at half rate; the audio
// clock is the master, so the tick would read a frame index racing ahead of the
// picture and advance to catch it -- section 29.3's "L forward is worse than
// slow", where video skipped 35 frames chasing sound. A "never skip a frame"
// violation outside the one sanctioned exception, from a comparison operator.
//
// So the rule stands as it has always been stated -- audio is 1x forward only,
// and every other rate is silent -- and 0.5x needs no new decision, only a
// predicate that says what the rule says.
bool MainWindow::audioShouldDrive() const {
    if (!audio_.hasAudio()) return false;
    if (!currentMedia_.has_value() || currentMedia_->kind != MediaKind::VideoFile) return false;
    const auto st = playback_.state();
    return st.mode == PlaybackMode::PlayingForward && std::abs(st.speed - 1.0) < 1e-4;
}

void MainWindow::startAudioForPlayback() {
    if (!audioShouldDrive() || !frameSource_) {
        stopAudio();
        return;
    }
    // Already running and locked: restarting would seek the device for nothing.
    if (audioDriving_ && audio_.isPlaying()) return;

    lastAudioClockS_ = -1.0;
    audioClockStalled_ = false;
    audioClockStall_.start();

    const double fps = std::max(1.0, frameSource_->fps());
    const double startSeconds = static_cast<double>(playback_.state().currentFrame) / fps;
    audio_.start(startSeconds);
    audioDriving_ = audio_.isPlaying();
}

void MainWindow::stopAudio() {
    audio_.stop();
    audioDriving_ = false;
}

// Called repeatedly by the I/O layer while a remote read is outstanding. The
// read itself is on a worker; this is what the UI thread does instead of
// sitting in a syscall. Cold LucidLink reads measured up to 1067ms, which as a
// blocking call is indistinguishable from a hung application.
void MainWindow::pumpDuringStorageStall(double waitedMs) {
    storageWaitMs_ = waitedMs;
    maxStorageWaitMs_ = std::max(maxStorageWaitMs_, waitedMs);

    if (!buffering_ && waitedMs >= kBufferingVisibleMs) {
        buffering_ = true;
        ++bufferingEvents_;
        bufferingClock_.start();
        if (overlay_) overlay_->setStorageState(QStringLiteral("BUFFERING"));
        // Repaint the indicator immediately rather than waiting for the next
        // natural paint, which may be a whole stall away.
        if (overlay_) overlay_->repaint();
    }

    // A short slice: long enough to be worth the trip through the event loop,
    // short enough that input latency stays well under a frame.
    QCoreApplication::processEvents(QEventLoop::AllEvents, 8);
}

void MainWindow::resetScrubLagModel() {
    pointerTrail_.clear();
    scrubDirection_ = 0;
    scrubReversals_ = 0;
    scrubPointerFps_ = 0.0;
    scrubPointerFramesTotal_ = 0;
    scrubLastPointerFrame_ = -1;
    scrubPresentedFrames_ = 0;
    scrubDecodeFps_ = 0.0;
    scrubLagMaxFrames_ = 0;
    scrubLagLastFrames_ = 0;
    scrubPointerToPreviewMs_ = 0.0;
    scrubPointerToPreviewMaxMs_ = 0.0;
    scrubWalkMaxFrames_ = 0;
    scrubSeeksAtGestureStart_ = hudPerf_.seekSamples;
    ctrlPointerFps_ = 0.0;
    ctrlLastPointerNs_ = -1;
    scrubStride_ = 1;
    scrubFramesSkipped_ = 0;
    scrubSampledSteps_ = 0;
    scrubGestureClock_.start();
}

long long MainWindow::computeScrubStride(long long gap) const {
    if (gap <= 1) return 1;
    if (!scrubSamplingEnabled()) return 1;

    // Sampling is only free where random access is free.
    //
    // On an all-intra codec a seek lands on the target and a strided step costs
    // exactly what an adjacent one costs, so showing every Nth frame is a
    // straight N-fold saving. On long-GOP it is the opposite: adjacent backward
    // steps are cache hits inside a GOP the decoder has already walked (~0.5ms),
    // and a strided step jumps out of that run and pays a seek plus a fresh GOP
    // walk (~46ms). Skipping frames there raises the cost per frame by far more
    // than it lowers the number of frames.
    //
    // Measured on 4K H.264 backward with this gate absent: cache hits 85.4% ->
    // 13.3%, decode 90.0 -> 13.9 f/s, 76 paints -> 14, stalls 7 of 73 -> 9 of
    // 10. And it runs away, because a higher stride lowers the measured decode
    // rate, which raises the stride: 1080p backward reached stride 14.
    //
    // Asked of the codec, not inferred from behaviour.
    //
    // Three inferred versions of this test were built and all three were wrong,
    // which is worth recording because the temptation to measure it is strong.
    // A latch on the first observed walk: ProRes seeks occasionally land short,
    // and one such walk disabled sampling for the session (4444 reversal set,
    // pointer-to-preview 22ms -> 1784ms). A decaying mean: collapses to zero
    // during a run of cache hits and declares a long-GOP file free. A mean per
    // request: diluted by forward steps that never seek, so any gesture with a
    // forward segment opened the gate during its backward stretches (4K H.264
    // reversals, stalls 2 of 437 -> 13 of 199). A mean per seek with an
    // evidence threshold: a forward ProRes sweep performs two seeks in total
    // and never reaches it, so 4444 went straight back to a 181-frame lag.
    //
    // The property wanted is exact and static -- does a seek land on the target
    // -- and the decoder has known it since open, where it already picks the
    // threading mode from it. `ra-walk` is kept in the HUD as the empirical
    // check on this answer rather than as the answer.
    if (!videoDecoder_.metadata().intraOnly) return 1;
    // No estimate yet -- the first frames of a gesture walk consecutively,
    // which is also the right answer for a drag that turns out to be slow.
    // Three presented frames is enough for the rate to mean anything and short
    // enough that sampling engages within the first few tens of ms.
    if (scrubPresentedFrames_ < 3 || scrubDecodeFps_ <= 0.0 || ctrlPointerFps_ <= 0.0) return 1;

    const double decodeFps = scrubDecodeFps_;
    // How many frames the pointer travels in the time one frame is decoded.
    // At or below 1 the decoder is keeping up and nothing may be skipped: this
    // is the guard that keeps every light-media case on the shipped path.
    const double perDecode = ctrlPointerFps_ / decodeFps;
    if (perDecode <= 1.0) return 1;

    long long stride = static_cast<long long>(std::ceil(perDecode * (1.0 + kScrubCatchUp)));
    stride = std::clamp<long long>(stride, 1, scrubMaxStride());
    // Never step past the pointer. Overshooting would put the picture ahead of
    // the hand, which reads as the image leading rather than following.
    return std::min(stride, gap);
}

long long MainWindow::computeScrubBatch(long long gap, long long stride) const {
    const long long cap = scrubBatchCap();
    if (cap <= 1) return 1;
    if (gap <= 1) return 1;

    // BATCHING AND SAMPLING MUST NOT COMPOUND, and the reason is not caution.
    // A batch covers CONSECUTIVE frames; a stride above 1 means the frames
    // wanted are not consecutive, so the two cannot describe the same set. They
    // also address different deficits: sampling exists because a frame is
    // expensive to DECODE on intra-only media, and no amount of amortised round
    // trip reaches that. Whenever section 15's gate has opened, this stays at 1.
    if (stride > 1) return 1;

    // The synchronous walk's ease, unchanged: cover a constant fraction of the
    // remaining distance, so the chain accelerates when far behind and settles
    // onto the target rather than arriving with a jolt. ceil(gap * 0.5) never
    // exceeds gap for gap >= 1, so a batch cannot step past the pointer.
    const long long eased = static_cast<long long>(
        std::ceil(static_cast<double>(gap) * kScrubEase));
    return std::clamp<long long>(eased, 1, cap);
}

void MainWindow::notePointerTarget(long long frame) {
    if (!scrubGestureClock_.isValid()) return;
    const qint64 nowNs = scrubGestureClock_.nsecsElapsed();

    // Distance travelled, not net displacement: a drag that goes out and comes
    // back has asked for every frame twice and the decoder had to follow it
    // both ways. Net would report a reversal as standing still.
    if (scrubLastPointerFrame_ >= 0) {
        const long long moved = std::llabs(frame - scrubLastPointerFrame_);
        scrubPointerFramesTotal_ += moved;
        const int dir = frame > scrubLastPointerFrame_ ? 1
                      : frame < scrubLastPointerFrame_ ? -1 : 0;
        if (dir != 0) {
            if (scrubDirection_ != 0 && dir != scrubDirection_) ++scrubReversals_;
            scrubDirection_ = dir;
        }
        // Short-window pointer rate for the sampling controller. Deliberately
        // not the cumulative figure: a drag that starts slow and then whips
        // must raise the stride now, not average it away.
        if (ctrlLastPointerNs_ >= 0 && moved > 0) {
            const double dtS = static_cast<double>(nowNs - ctrlLastPointerNs_) / 1'000'000'000.0;
            if (dtS > 0.0005) {
                const double fps = static_cast<double>(moved) / dtS;
                ctrlPointerFps_ = ctrlPointerFps_ > 0.0
                    ? ctrlPointerFps_ + 0.3 * (fps - ctrlPointerFps_) : fps;
            }
        }
        if (moved > 0) ctrlLastPointerNs_ = nowNs;
    } else {
        ctrlLastPointerNs_ = nowNs;
    }
    scrubLastPointerFrame_ = frame;

    pointerTrail_.push_back(PointerSample{frame, nowNs});
    while (pointerTrail_.size() > kPointerTrailMax) pointerTrail_.pop_front();

    const double elapsedS = static_cast<double>(nowNs) / 1'000'000'000.0;
    if (elapsedS > 0.05) {
        scrubPointerFps_ = static_cast<double>(scrubPointerFramesTotal_) / elapsedS;
    }
}

void MainWindow::notePresentedScrubFrame(long long frame) {
    if (!scrubGestureClock_.isValid()) return;
    const qint64 nowNs = scrubGestureClock_.nsecsElapsed();

    ++scrubPresentedFrames_;
    const double elapsedS = static_cast<double>(nowNs) / 1'000'000'000.0;
    if (elapsedS > 0.05) {
        scrubDecodeFps_ = static_cast<double>(scrubPresentedFrames_) / elapsedS;
    }

    if (pendingScrubFrame_ >= 0) {
        scrubLagLastFrames_ = std::llabs(pendingScrubFrame_ - frame);
        scrubLagMaxFrames_ = std::max(scrubLagMaxFrames_, scrubLagLastFrames_);
    }

    // How long ago the hand was where the picture now is. Scanned NEWEST-first:
    // on a gesture that crosses the same frame several times the user is asking
    // for it now, not on the first pass, and measuring from the first pass
    // charges this frame with the whole history of the drag -- a 4444 reversal
    // set reported 1537ms that way against 22ms on a single sweep, which is an
    // artefact of the metric and not of the picture. Nearest rather than exact,
    // because a sampled preview shows frames the pointer stepped over.
    long long bestDelta = -1;
    qint64 bestNs = 0;
    for (auto it = pointerTrail_.rbegin(); it != pointerTrail_.rend(); ++it) {
        const auto& sample = *it;
        const long long delta = std::llabs(sample.frame - frame);
        if (bestDelta < 0 || delta < bestDelta) {
            bestDelta = delta;
            bestNs = sample.ns;
        }
    }
    if (bestDelta >= 0) {
        const double ms = static_cast<double>(nowNs - bestNs) / 1'000'000.0;
        scrubPointerToPreviewMs_ = ms;
        scrubPointerToPreviewMaxMs_ = std::max(scrubPointerToPreviewMaxMs_, ms);
    }

    scrubWalkMaxFrames_ = std::max(scrubWalkMaxFrames_, hudPerf_.lastWalkFrames);
    // Only a request that actually sought says anything about the cost of
    // random access. Detected by the decoder's seek counter moving, so a cache
    // hit and a sequential step both correctly count for nothing.
    if (hudPerf_.seekSamples > mediaSeeksSeen_) {
        mediaSeeksSeen_ = hudPerf_.seekSamples;
        mediaWalkFramesTotal_ += hudPerf_.lastWalkFrames;
        ++mediaSeekCount_;
    }
}

void MainWindow::startUiServiceMeasurement() {
    // Reset per gesture: the interesting figure is the worst gap during *this*
    // drag, not the worst since the app launched.
    uiServiceGapMaxMs_ = 0.0;
    uiServiceGapSumMs_ = 0.0;
    uiServiceSamples_ = 0;
    uiServiceGapsOver_ = 0;
    uiServiceLastNs_ = -1;
    uiServiceClock_.start();
    uiServiceTimer_.start();
}

void MainWindow::stopUiServiceMeasurement() {
    uiServiceTimer_.stop();
    uiServiceLastNs_ = -1;
}

bool MainWindow::isVideoScrubActive() const {
    return currentMedia_.has_value() && currentMedia_->kind == MediaKind::VideoFile;
}

void MainWindow::queueVideoScrubFrame(long long frameIndex) {
    // The user has moved on. Supersede anything still in flight so its frame is
    // discarded rather than presented after this one. Bumped on every change of
    // target, not only when storage is busy: "the target moved" is the
    // condition the async worker will act on, and it must mean the same thing
    // whether or not a read happens to be outstanding right now.
    if (frameIndex != pendingScrubFrame_) supersedeInFlightRequests();
    pendingScrubFrame_ = frameIndex;
    notePointerTarget(frameIndex);
    playback_.setCurrentFrame(frameIndex);

    // If a coalescing window is open, the timer picks up the latest pending
    // frame when it fires. Otherwise respond immediately (snappy first frame),
    // then open a window so rapid slider moves coalesce instead of stacking
    // one synchronous decode per event.
    if (scrubTimer_.isActive()) return;
    flushVideoScrub(false);
    scrubTimer_.start(kScrubCoalesceMs);
}

void MainWindow::postScrubStep(long long frame, int direction, long long batch) {
    if (!isVideoScrubActive() || frame < 0) return;
    // One request in flight at a time. This is not a queue depth limit for its
    // own sake: the shuttle's next target is derived from the frame that was
    // last PRESENTED, so there is nothing meaningful to ask for until the
    // outstanding one lands. The chain re-posts from onScrubResult.
    if (scrubWorker_.busy()) return;
    // The UI thread is inside a decode of its own (a remote read pumping the
    // event loop re-entered us). Leave it; the coalescing timer comes back.
    if (storageBusy_) {
        if (!scrubTimer_.isActive()) scrubTimer_.start(kScrubCoalesceMs);
        return;
    }
    grantDecoderLease();

    trace::core::ScrubRequest request;
    request.frame = frame;
    request.direction = direction;
    // Stamped with the generation as it stands NOW. The worker's staleness
    // test is against the generation last posted to it rather than against
    // this counter directly, and the difference is load-bearing:
    // requestGeneration_ bumps on every pointer move, while the shuttle's
    // target only changes when the direction reverses. Testing the counter
    // itself would abandon a perfectly good walk step on every mouse move.
    request.generation = requestGeneration_;
    // Consecutive frames from `frame`, all of them decoded and all of them
    // presented in order. The budget is the synchronous walk's, deliberately:
    // it is the same quantity -- how long one shuttle slice may spend decoding
    // -- and reusing the number keeps the two paths comparable under
    // TRACE_ASYNC_SCRUB rather than each having its own. What it bounds differs,
    // and that difference is why it is safe here: on the UI thread it capped
    // occupancy, on the worker it caps how long the picture waits for the first
    // frame of a batch. On ProRes 4444 a frame is ~23ms, so the first one
    // exhausts it and the batch collapses to 1 with no media-conditional branch.
    request.batch = std::max<long long>(1, batch);
    request.batchBudgetMs = scrubWalkBudgetMs();
    scrubInFlightDir_ = direction;
    // What this step actually skipped over, recorded where the decision is
    // made rather than recomputed from the result -- the two can differ if the
    // request lands off-target and only the honest one is worth reporting.
    if (activeScrubFrame_ >= 0) {
        const long long step = std::llabs(frame - activeScrubFrame_);
        scrubStride_ = std::max<long long>(1, step);
        if (step > 1) {
            scrubFramesSkipped_ += step - 1;
            ++scrubSampledSteps_;
        }
    }
    scrubWorker_.post(request);
}

void MainWindow::onScrubResult() {
    trace::core::ScrubResult result;
    bool presentedAny = false;
    bool hardError = false;
    bool landed = false;
    LandingKind landedKind = LandingKind::None;
    scrubInFlightDir_ = 0;

    while (scrubWorker_.takeResult(result)) {
        // Telemetry first, and unconditionally: a dropped result still says
        // what the decoder did, and the HUD should show the work that was
        // done rather than only the work that was used.
        hudPerf_ = result.perf;
        for (int i = 0; i < static_cast<int>(trace::core::IoPhase::Count); ++i) {
            hudIo_[i] = result.io[i];
        }

        // The delivery boundary. The worker checked staleness before
        // publishing, but the generation can move in the gap between that
        // check and this callback being serviced -- a release, a play, a new
        // file -- and the check that matters is the last one before the frame
        // reaches the viewer.
        if (result.generation != scrubWorker_.latestGeneration()) {
            ++supersededResults_;
            continue;
        }
        if (result.abandoned) {
            // Not an error and not a frame: the target moved while this was
            // being decoded. Counted with the stale drops because that is what
            // it is -- work thrown away because the user moved on.
            ++supersededResults_;
            continue;
        }
        // The exact landing. Taken before the failure branch below, not after,
        // because a landing owns its own failure: a step that could not be
        // decoded has a playhead to put back, and `hardError` means something
        // else entirely -- it re-arms the drag's coalescing timer, which a
        // landing has no business doing.
        if (landingPending_ && result.generation == landingGeneration_
            && result.requestedFrame == landingFrame_) {
            landedKind = landingKind_;
            completeExactLanding(result);
            landed = true;
            continue;
        }

        if (!result.ok) {
            // The playback prefetch reaching the real end of the media. Only
            // long-GOP media gets here -- the decoder is exhausted before the
            // frame count says it should be -- and on this path it arrives as a
            // result rather than as a return value. Mark it and stop asking; the
            // tick's end-of-media branch does the rest once the queue drains.
            if (playbackPrefetchActive_) {
                playbackPrefetchExhausted_ = true;
                playbackPrefetchNext_ = -1;
                continue;
            }
            if (!result.error.isEmpty()) statusBar()->showMessage(result.error, 3000);
            hardError = true;
            continue;
        }

        // Playback prefetch results are QUEUED, not presented -- the same rule
        // and the same reason as the shuttle below. The tick owns when a frame
        // goes on screen; this callback only owns keeping the pipeline fed.
        if (playbackPrefetchActive_) {
            playbackQueue_.push_back({result.requestedFrame, result.frame});
            pqMaxDepth_ = std::max<long long>(
                pqMaxDepth_, static_cast<long long>(playbackQueue_.size()));
            pqBytes_ = playbackQueueBytes();
            pqPeakBytes_ = std::max(pqPeakBytes_, pqBytes_);
            pumpPlaybackQueue();
            continue;
        }

        // Reverse shuttle results are QUEUED, not presented. The playback tick
        // owns when a frame goes on screen; this callback only owns keeping the
        // pipeline fed. Presenting here would put reverse back on the decoder's
        // cadence instead of the scheduler's, which is the whole thing the queue
        // exists to prevent.
        if (shuttleRunActive_) {
            // Learn where the keyframes ARE, from a request that just told us.
            // A request for frame T that walked W frames landed on the keyframe
            // at T - W, which is an exact position. Two consecutive positions
            // give the exact spacing; no statistic is involved.
            const long long walked = result.perf.lastWalkFrames;
            if (walked >= 0 && result.requestedFrame >= 0) {
                const long long kf = result.requestedFrame - walked;
                if (kf >= 0) {
                    if (shuttleKfAnchor_ < 0) {
                        shuttleKfAnchor_ = kf;
                    } else if (kf < shuttleKfAnchor_) {
                        const long long gap = shuttleKfAnchor_ - kf;
                        // Ignore a gap of 1: consecutive keyframes that close
                        // together mean this is not a long-GOP region and
                        // snapping to it would buy nothing.
                        if (gap > 1) shuttleGop_ = gap;
                        shuttleKfAnchor_ = kf;
                    }
                }
            }
            shuttleQueue_.push_back({result.requestedFrame, result.frame});
            shuttleQueueMaxSeen_ =
                std::max<long long>(shuttleQueueMaxSeen_,
                                    static_cast<long long>(shuttleQueue_.size()));
            pumpShuttleQueue();
            continue;
        }

        // Present. Identical to the synchronous walk's loop body, minus the
        // decode: the frame is already here.
        videoFrameBuffer_ = result.frame;
        lastRequestedFrame_ = result.requestedFrame;
        lastDeliveredFrame_ = result.frame.frameIndex;
        playback_.setCurrentFrame(result.frame.frameIndex);
        viewer_->setFrame(videoFrameBuffer_);

        const qint64 nowNs = scrubPresentClock_.isValid()
            ? scrubPresentClock_.nsecsElapsed() : 0;
        if (!scrubPresentClock_.isValid()) scrubPresentClock_.start();
        // repaint(), not update(): update() coalesces, and a chain of them
        // would decode every frame and display only the last, which is a jump
        // and is the behaviour the shuttle exists to remove.
        viewer_->repaint();
        if (scrubLastPresentNs_ >= 0) {
            const double gapMs =
                static_cast<double>(nowNs - scrubLastPresentNs_) / 1'000'000.0;
            const QScreen* scr = screen();
            const double refreshHz = (scr && scr->refreshRate() > 1.0)
                ? scr->refreshRate() : 60.0;
            const double refreshMs = 1000.0 / refreshHz;
            scrubPaintGapLastMs_ = gapMs;
            scrubPaintGapMaxMs_ = std::max(scrubPaintGapMaxMs_, gapMs);
            scrubPaintGapSumMs_ += gapMs;
            ++scrubPaintGapSamples_;
            if (gapMs < refreshMs) ++scrubPaintsWasted_;
            if (gapMs > refreshMs * 2.0) ++scrubPaintStalls_;
            if (gapMs > kScrubHitchMs) ++scrubPaintHitches_;
        }
        scrubLastPresentNs_ = nowNs;

        activeScrubFrame_ = result.frame.frameIndex;
        notePresentedScrubFrame(activeScrubFrame_);
        // Shuttled, so possibly a preview-resolution frame: the release must
        // land this properly even if it is already the one on screen.
        scrubShownExact_ = false;
        presentedAny = true;

        // Measured rather than derived from VideoPerfStats averages, which
        // pool seek-walk decodes and read several times the true sequential
        // cost. Same EMA as the synchronous walk so the HUD figure is
        // comparable across the two paths.
        scrubWalkPerFrameMs_ += 0.35 * (result.decodeMs - scrubWalkPerFrameMs_);
        scrubWalkPerFrameMs_ = std::max(0.05, scrubWalkPerFrameMs_);

        // Decode cost for the sampling controller. Cost is averaged and the
        // rate derived from it, not the other way round: a cache hit is 0.1ms,
        // i.e. 10000 f/s, and averaging rates would let one hit dominate and
        // collapse the stride back to 1 just as a heavy run begins.
    }

    // The landing is complete, so the decoder comes home. Outside the drain
    // loop because reclaimDecoder() clears the result deque that loop is
    // iterating; and before resumePlaybackAfterScrub() because playback decodes
    // synchronously on this thread and cannot run under a lease.
    //
    // That is the same ordering the synchronous release had -- land, reclaim,
    // resume -- and it is the reason the resume could not simply stay where it
    // was in sliderReleased(). Moving the landing off this thread moved the
    // moment the landing FINISHES with it, and the resume is pinned to the
    // finish rather than to the release.
    if (landed) {
        reclaimDecoder();
        if (landedKind == LandingKind::ScrubRelease) {
            resumePlaybackAfterScrub();
            refreshHud("Scrub Release");
        } else if (landedKind == LandingKind::Step) {
            if (currentMedia_.has_value()
                && currentMedia_->kind == MediaKind::ImageSequence) {
                prefetchNeighbors();
            }
            refreshHud(landingHudLabel_);
        } else {
            refreshHud("Scrub");
        }
        return;
    }

    // Chain. Any outcome that did not advance the picture re-posts the same
    // frame, so a dropped or abandoned result resumes the shuttle rather than
    // silently ending it -- which would leave the picture stranded behind a
    // pointer that has stopped moving.
    if (scrubbing_ && pendingScrubFrame_ >= 0 && activeScrubFrame_ >= 0
        && pendingScrubFrame_ != activeScrubFrame_) {
        if (hardError) {
            // Do not spin on a decode that is failing for real. Let the
            // coalescing timer retry at its own pace.
            if (!scrubTimer_.isActive()) scrubTimer_.start(kScrubCoalesceMs);
        } else {
            const int dir = pendingScrubFrame_ > activeScrubFrame_ ? 1 : -1;
            const long long gap = std::llabs(pendingScrubFrame_ - activeScrubFrame_);
            const long long stride = computeScrubStride(gap);
            postScrubStep(activeScrubFrame_ + dir * stride, dir,
                          computeScrubBatch(gap, stride));
        }
    }

    if (presentedAny || hardError) refreshHud("Scrub");
}

void MainWindow::flushVideoScrub(bool forceExact) {
    // Storage is mid-read. Re-arm rather than drop the scrub: the pending
    // frame is already the newest target, so latest-target-wins is preserved
    // and the drag stays responsive instead of losing the release.
    if (storageBusy_) {
        if (!scrubTimer_.isActive()) scrubTimer_.start(kScrubCoalesceMs);
        return;
    }
    scrubTimer_.stop();

    if (!isVideoScrubActive()) {
        pendingScrubFrame_ = -1;
        activeScrubFrame_ = -1;
        return;
    }

    if (pendingScrubFrame_ < 0) {
        return;
    }

    // A LANDING IS IN FLIGHT AND THIS IS NOT THE RELEASE. Wait for it.
    //
    // The hole this closes is specific and it re-freezes the window on exactly
    // the file the change is for. A press posts its landing and returns with
    // activeScrubFrame_ still at -1, because that field means "what is on
    // screen" and nothing has arrived yet. If the pointer then moves before the
    // landing lands -- a third of a second on the Seedance clip, so most drags
    // that begin with a click -- the drag branch below requires walkFrom >= 0,
    // fails that test, and falls through to the synchronous block. That block
    // calls reclaimDecoder(), which CANCELS the landing, and then decodes the
    // same walk on this thread. Everything this change removed comes straight
    // back, in the middle of a gesture rather than at its start.
    //
    // Deferring costs the drag nothing it was not already going to pay: the
    // landing is decoding a frame, the window stays alive through it, and
    // completeExactLanding() re-arms this timer so the walk resumes from the
    // frame that actually arrived. The release is exempt because it must always
    // be able to move the target -- it adopts an in-flight landing for the same
    // frame and supersedes one for any other.
    if (landingPending_ && !forceExact) {
        if (!scrubTimer_.isActive()) scrubTimer_.start(kScrubCoalesceMs);
        return;
    }

    const long long targetFrame = pendingScrubFrame_;
    if (!forceExact && targetFrame == activeScrubFrame_) {
        return;
    }
    // The press already landed this exact frame, full-res and accurate. A click
    // arrives as press then release on the same value, so without this the
    // release paid a second seek and decode of a frame already on screen -- on
    // 4K ProRes 4444 that is another ~25ms in front of a picture the user can
    // already see. Only skipped when the frame is known exact: a soft preview
    // or a shuttled frame must still be re-landed.
    if (forceExact && targetFrame == activeScrubFrame_ && scrubShownExact_) {
        pendingScrubFrame_ = -1;
        refreshHud("Scrub Release");
        return;
    }

    // Mid-drag frames use Scrub (fast, half-res preview at >=1920px). The
    // landing frame â€” slider release or a jump while not dragging â€” uses Step
    // so the frame being inspected is full-res and accurately converted.
    //
    // A press is not yet a drag. The slider sets its value absolutely on a
    // groove click, so the first value after a press is where the user pointed,
    // not somewhere they dragged through -- shuttling to it walks every frame
    // in between, which is the wrong answer to "go there" and on heavy media is
    // most of a second of decoding before the frame appears. Land the press
    // exactly; the shuttle starts from there if the pointer then moves.
    const bool dragging = !forceExact && scrubbing_ && !scrubJumpPending_;
    scrubJumpPending_ = false;
    const auto mode = dragging
        ? trace::core::VideoDecoderFFmpeg::RequestMode::Scrub
        : trace::core::VideoDecoderFFmpeg::RequestMode::Step;

    // Dragging forward is a shuttle, not a sample: walk the decoder through
    // every frame between here and the pointer and put each one on screen.
    // A click still jumps, because a click arrives as press+release and the
    // release forces the exact target through the Step path above.
    //
    // This is affordable because it inverts what used to be paid for. Seeking
    // was the expensive half (a keyframe landing plus a GOP walk); stepping
    // forward is ~1ms at 1080p. The old path seeked on every drag update and
    // showed the keyframe it landed on, so a drag displayed one new picture per
    // GOP while claiming to be exact.
    //
    // The budget decides the policy by itself, which is what makes this work
    // across codecs: at 1080p H.264 a frame costs ~1.3ms so a slice covers
    // several frames and a drag shuttles through all of them; at 4K ProRes a
    // frame costs ~20ms so maxWalk collapses to 1, every drag update is a jump,
    // and the picture stays on the pointer instead of falling behind it. Heavy
    // media therefore degrades to "as many frames as fit, always current"
    // rather than to "wrong frame, instantly".
    // activeScrubFrame_ tracks what is actually ON SCREEN, so it is only
    // advanced where a frame has genuinely been presented. It used to be
    // assigned the target up front, which meant a walk that exited without
    // completing an iteration -- a decode failure, a re-entrancy bail, a pacing
    // break on the first step -- left it claiming to have arrived. The next
    // slice then computed its gap from a position the viewer had never shown,
    // so the walk silently skipped everything in between and `lag` read 0 while
    // frames were being missed.
    const long long walkFrom = activeScrubFrame_;

    QString error;
    if (dragging && walkFrom >= 0 && targetFrame != walkFrom && asyncScrubEnabled()) {
        // Asynchronous shuttle. Same rule as the synchronous walk below and
        // the same frames in the same order -- one frame at a time toward the
        // pointer, never a jump -- but the decode happens on the worker and
        // this thread returns to the event loop immediately.
        //
        // THE EASE AND THE WALK BUDGET ARE BACK, AND THE REASONING THAT
        // DROPPED THEM WAS WRONG IN ONE TERM. It ran: both existed only to
        // decide when the synchronous loop should yield, yielding is now free
        // and happens after every frame, so "accelerate when far behind,
        // settle gently on arrival" falls out of the pipeline by itself --
        // the chain runs as fast as frames can be decoded.
        //
        // The chain does not run as fast as frames can be decoded. It runs at
        // one CROSS-THREAD ROUND TRIP per frame, and that is a fixed cost the
        // frame does not pay for. Measured on a 1280x720 H.264 source: 0.12ms
        // to decode a frame, 3.56ms to deliver one, so 97% of the interval was
        // the round trip and the picture ran 76 frames and 236ms behind the
        // pointer while the decoder was idle. It held at 4K (3.87ms a frame)
        // and at 1080p (0.68) purely because the frame was expensive enough to
        // hide it, and those are the two files it was checked on.
        //
        // So `ceil(gap * kScrubEase)` decides how many CONSECUTIVE frames one
        // request covers, and `kScrubWalkBudgetMs` bounds it in time. Every
        // frame is still decoded, delivered and presented individually and in
        // order; what is batched is the asking. Nothing is sampled or skipped.
        const int dir = targetFrame > walkFrom ? 1 : -1;

        // The pointer has crossed the picture. Whatever is in flight is now a
        // frame on the far side of where the user is going, so continuing to
        // decode it is pure waste -- and if it missed the cache it is a seek
        // plus a GOP walk, up to ~100ms of it, in front of a reversal the user
        // has already made. Supersede so the checkpoint inside the walk gives
        // up; the chain re-posts in the new direction when the abandoned
        // result comes back.
        //
        // This is the ONLY place a pointer move invalidates work in flight,
        // and the condition is deliberately narrow: not "the pointer moved"
        // -- which is every mouse event and was measured abandoning 62% of all
        // walks -- but "the pointer is now on the other side of the picture".
        if (scrubWorker_.busy() && scrubInFlightDir_ != 0 && scrubInFlightDir_ != dir) {
            scrubWorker_.supersede(supersedeInFlightRequests());
        }
        const long long gap = std::llabs(targetFrame - walkFrom);
        const long long stride = computeScrubStride(gap);
        postScrubStep(walkFrom + dir * stride, dir, computeScrubBatch(gap, stride));
        refreshHud("Scrub");
        return;
    }

    if (dragging && walkFrom >= 0 && targetFrame != walkFrom) {
        // A drag NEVER jumps, in either direction. Frames are shown
        // consecutively however far the pointer has run ahead, and the picture
        // is allowed to trail it. Snapping to the pointer instead was tried and
        // felt harsh: a fast drag skipped whole runs of frames, which reads as
        // tearing through the clip rather than shuttling it.
        //
        // Backward is the same walk with the sign flipped, and it is only
        // affordable because of the reverse cache. A backward step that misses
        // costs a seek plus a GOP walk, but that walk fills the cache on its
        // way through, so one miss is followed by a run of hits covering the
        // rest of the GOP. The time budget absorbs the miss (one frame that
        // slice, then re-arm) rather than letting it stall the drag.
        //
        // How far a slice advances is eased rather than fixed. Covering a
        // constant fraction of the remaining distance gives an exponential
        // approach: a long way behind and it moves fast, close and it settles
        // gently onto the target instead of arriving with a jolt. Combined
        // with the time budget below the two limits swap over naturally --
        // budget-bound while far away, ease-bound as it converges -- so the
        // motion accelerates and decelerates without either being scheduled.
        const int dir = targetFrame > walkFrom ? 1 : -1;
        const long long gap = std::llabs(targetFrame - walkFrom);
        const long long desired = std::max<long long>(1,
            static_cast<long long>(std::ceil(static_cast<double>(gap) * kScrubEase)));

        const double kScrubWalkBudgetMs = scrubWalkBudgetMs();
        // Decode cadence and paint cadence are separate things, and conflating
        // them is what made a fast drag feel bad while every throughput number
        // said it was fine.
        //
        // The decoder should walk as fast as it can: that is what closes the
        // gap to the pointer. The *painting* should happen no faster than the
        // panel refreshes, because a repaint inside one refresh interval simply
        // overwrites the previous one and nothing is shown. Measured on 4K
        // H.264 with a scripted fast scrub: 616 of 627 paints -- 98% -- landed
        // sooner than the display could sample the one before, and the motion
        // arrived as bursts separated by stalls of up to 102ms.
        //
        // Skipping those repaints does not skip frames from the viewer's point
        // of view: every frame is still decoded, in order, and the display
        // samples that sequence at its own rate. It samples it *evenly* now
        // rather than catching whichever frame happened to be in the buffer at
        // scan-out. The frames not painted were never visible either way.
        //
        // This is NOT the old TRACE_SCRUB_PACE, which broke out of the walk and
        // re-armed a timer per frame -- that throttled the decoder too and
        // measured 151 paints against 631, 45/sec, with the drag unable to
        // finish a sweep. Nothing here interrupts the walk.
        const QScreen* scr = screen();
        const double refreshHz = (scr && scr->refreshRate() > 1.0)
            ? scr->refreshRate() : 60.0;
        const double minPresentIntervalMs = (1000.0 / refreshHz) * scrubPaceFraction();
        if (!scrubPresentClock_.isValid()) scrubPresentClock_.start();

        QElapsedTimer walkTimer;
        walkTimer.start();
        prepareVideoRequest(mode, dir, true);
        long long walked = 0;
        const long long steps = std::min(gap, desired);
        for (long long i = 1; i <= steps; ++i) {
            const long long f = walkFrom + dir * i;
            // A remote read pumped the event loop and something re-entered;
            // drop out and let the re-armed timer resume from here.
            if (storageBusy_) break;
            playback_.setCurrentFrame(f);
            if (!loadCurrentFrame(error, mode)) break;

            // Decode always; paint only when the display could show it. The
            // walk is never interrupted for pacing -- it just declines to draw
            // frames that would be overwritten before a refresh sampled them.
            // The last frame of the slice is always drawn, so whatever the
            // decoder reached is what is on screen when the slice ends.
            const qint64 nowNs = scrubPresentClock_.nsecsElapsed();
            bool paintDue = true;
            if (scrubLastPresentNs_ >= 0 && minPresentIntervalMs > 0.0) {
                const double sinceMs =
                    static_cast<double>(nowNs - scrubLastPresentNs_) / 1'000'000.0;
                paintDue = sinceMs >= minPresentIntervalMs;
            }
            // Arrived: this is the frame the pointer is on and no further slice
            // will run, so it must be drawn whether or not a refresh is due.
            // Mid-walk frames are not force-drawn even at slice end -- a
            // converged shuttle does one frame per slice, so forcing there
            // painted every frame again and put the wasted rate straight back
            // to 90%.
            const bool arrived = (f == targetFrame);
            if (paintDue || arrived) {
                // repaint(), not update(): update() coalesces, so a loop of them
                // would decode every frame and display only the last -- which is
                // a jump, and is the behaviour the shuttle exists to remove.
                viewer_->repaint();
                if (scrubLastPresentNs_ >= 0) {
                    const double gapMs =
                        static_cast<double>(nowNs - scrubLastPresentNs_) / 1'000'000.0;
                    scrubPaintGapLastMs_ = gapMs;
                    scrubPaintGapMaxMs_ = std::max(scrubPaintGapMaxMs_, gapMs);
                    scrubPaintGapSumMs_ += gapMs;
                    ++scrubPaintGapSamples_;
                    const double refreshMs = 1000.0 / refreshHz;
                    if (gapMs < refreshMs) ++scrubPaintsWasted_;
                    if (gapMs > refreshMs * 2.0) ++scrubPaintStalls_;
                    if (gapMs > kScrubHitchMs) ++scrubPaintHitches_;
                }
                scrubLastPresentNs_ = nowNs;
            }
            activeScrubFrame_ = f;
            // Measured on the synchronous path too, so the two stay comparable
            // under TRACE_ASYNC_SCRUB=0 rather than the lag model only existing
            // for one of them.
            notePresentedScrubFrame(f);
            // Shuttled, so possibly a half-res preview: the release must land
            // this frame properly even if it is already the one on screen.
            scrubShownExact_ = false;
            ++walked;
            if (static_cast<double>(walkTimer.nsecsElapsed()) / 1'000'000.0
                    >= kScrubWalkBudgetMs) {
                break;
            }
        }
        // Kept as telemetry: it is what the HUD reports the shuttle rate from,
        // and it is the number to look at first if a drag ever feels slow.
        if (walked > 0) {
            const double perFrame =
                (static_cast<double>(walkTimer.nsecsElapsed()) / 1'000'000.0)
                / static_cast<double>(walked);
            scrubWalkPerFrameMs_ += 0.35 * (perFrame - scrubWalkPerFrameMs_);
            scrubWalkPerFrameMs_ = std::max(0.05, scrubWalkPerFrameMs_);
        }
        // Still behind: come straight back for the next slice rather than
        // waiting out the coalescing interval. That interval exists to stop
        // rapid slider events stacking one decode each; it has no business
        // throttling catch-up. Leaving it in place capped the shuttle at a
        // slice per 12ms *plus* the slice's own ~8-10ms of work, so roughly 45
        // slices a second, and a quick drag outran it and trailed further and
        // further behind. Zero-interval still goes through the event loop, so
        // pointer moves and repaints are serviced between slices.
        if (activeScrubFrame_ != targetFrame) {
            scrubTimer_.start(scrubRearmMs());
        }
        if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
        refreshHud("Scrub");
        return;
    }

    // The exact landing -- a groove click's press, a release, or a jump made
    // while not dragging. This is the block the owner's 2026-08-14 ruling is
    // about: it decoded on this thread, and on a file whose every miss walks
    // from the head of the clip that is 261-585ms during which the window
    // answers nothing. Posted to the worker now; the frame is presented and the
    // gesture finished in onScrubResult().
    //
    // The direction handed over is which way the playhead is MOVING, so a
    // backward click reaches the decoder as a backward request exactly as the
    // synchronous path's prepareVideoRequest(mode, 1, true) never did -- that
    // passed a hard 1 regardless. Kept honest here because the worker sets the
    // decoder's playback direction from it.
    if (mode == trace::core::VideoDecoderFFmpeg::RequestMode::Step) {
        const int dir = (activeScrubFrame_ >= 0 && targetFrame < activeScrubFrame_) ? -1 : 1;
        const LandingKind kind = forceExact ? LandingKind::ScrubRelease
                                            : LandingKind::ScrubPress;
        if (requestExactFrameAsync(targetFrame, dir, kind)) {
            landingStepDelta_ = 0;
            refreshHud(forceExact ? "Scrub Release" : "Scrub");
            return;
        }
    }

    ++landingsSync_;
    playback_.setCurrentFrame(targetFrame);
    prepareVideoRequest(mode, 1, true);
    if (loadCurrentFrame(error, mode)) {
        activeScrubFrame_ = targetFrame;
        scrubShownExact_ = (mode == trace::core::VideoDecoderFFmpeg::RequestMode::Step);
    } else if (!error.isEmpty()) {
        statusBar()->showMessage(error, 3000);
    }

    // Refresh here too: the scrub branch of valueChanged returns without
    // touching the HUD, so mid-drag preview state (exact vs approximate,
    // walk distance) was previously invisible until the slider was released.
    refreshHud(forceExact ? "Scrub Release" : "Scrub");

    if (forceExact || !scrubbing_) {
        pendingScrubFrame_ = -1;
    } else if (pendingScrubFrame_ != activeScrubFrame_) {
        scrubTimer_.start(kScrubCoalesceMs);
    }
}

void MainWindow::openMediaPath(const QString& path) {
    const QFileInfo fi(path);
    if (path.isEmpty() || !fi.exists() || !fi.isFile()) {
        statusBar()->showMessage("Ignored command-line path: " + path, 3000);
        return;
    }
    openPath(fi.absoluteFilePath());
}

void MainWindow::refreshHud(const QString& action) {
    const auto st = playback_.state();

    // Any move off the frame playback stopped on -- a step, a scrub, a new
    // file -- means the playhead is no longer parked at the end, so Play should
    // resume from where it now is rather than rewinding. This runs after every
    // transport action, which is why the flag is cleared here rather than at
    // each of the dozen sites that can move the playhead.
    if (playbackAtEnd_ && st.currentFrame != playbackEndFrame_) {
        playbackAtEnd_ = false;
        playbackEndFrame_ = -1;
    }

    // Hidden means NOT BUILT. Everything below this point formats strings for
    // overlay_, and overlay_ is the widget H just hid -- several hundred bytes of
    // QString construction on the UI thread, on every transport action and every
    // playback tick, for a widget nobody can see. Building it anyway is what the
    // pre-existing Return binding did, so "hide the HUD" cost exactly nothing
    // before this; the owner's reason for wanting the toggle is to judge feel
    // without the instrument, which means without its cost.
    //
    // The telemetry capture still runs, and that is deliberate. It is a struct
    // copy, and it feeds `ra-walk` and the seek counters, which must not come to
    // mean something different depending on whether the HUD happened to be
    // visible. A counter that changes what it measures when an unrelated toggle
    // moves is the exact failure this project keeps re-learning.
    if (!viewState_.showHud) {
        if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::VideoFile) {
            captureDecoderTelemetry();
        }
        syncTransportBar();
        return;
    }

    QString mode = "Empty";
    switch (st.mode) {
        case PlaybackMode::Paused: mode = "Paused"; break;
        case PlaybackMode::PlayingForward: mode = "Play >"; break;
        case PlaybackMode::PlayingReverse: mode = "Play <"; break;
        default: break;
    }

    QString line = "No media loaded";
    QString primaryReadout;

    const double fps = frameSource_ ? frameSource_->fps() : 24.0;
    const double sec = trace::core::TimeFormat::frameToSeconds(st.currentFrame, fps);
    const QString elapsed = trace::core::TimeFormat::frameToElapsed(st.currentFrame, fps);

    switch (viewState_.readoutMode) {
        case PrimaryReadoutMode::Frame:
            primaryReadout = QString("Frame: %1").arg(st.currentFrame);
            break;
        case PrimaryReadoutMode::Seconds:
            primaryReadout = QString("Seconds: %1")
                                 .arg(trace::core::TimeFormat::formatSeconds(sec));
            break;
        case PrimaryReadoutMode::Elapsed:
            primaryReadout = QString("Elapsed: %1").arg(elapsed);
            break;
        case PrimaryReadoutMode::Timecode:
            // Reachable only when the source carries a timecode --
            // setReadoutMode refuses the mode otherwise and openPath resets it
            // when new media has none. The fallback is still written out rather
            // than asserted, because a readout that silently prints the wrong
            // kind of value is exactly what this phase exists to remove.
            primaryReadout = hasSourceTimecode_
                ? QString("Timecode: %1").arg(sourceTimecodeAt(st.currentFrame))
                : QString("Elapsed: %1").arg(elapsed);
            break;
    }

    if (currentMedia_.has_value()) {
        if (currentMedia_->kind == MediaKind::VideoFile) {
            // metadata_ is written only by open(), which cannot run while a
            // lease is out, so it is immutable for the lease's lifetime and
            // safe to read directly. Everything else comes from the snapshot:
            // while the worker holds the decoder its counters are being
            // written by that thread, and the HUD has no business reading
            // them. Refreshed from the live decoder here whenever this thread
            // owns it, which is every case except mid-drag.
            captureDecoderTelemetry();
            const auto& vm = videoDecoder_.metadata();
            const auto& perf = hudPerf_;
            const auto& drawPerf = viewer_->perfStats();
            const double reverseHitRate = perf.reverseCacheLookups > 0
                ? (100.0 * static_cast<double>(perf.reverseCacheHits) / static_cast<double>(perf.reverseCacheLookups))
                : 0.0;

            // Three grouped lines: a single line overflowed the window at 4K
            // and clipped exactly the fields needed to diagnose playback
            // (rev-hit, late, walk).
            const double budgetMs = vm.fps > 0.0 ? 1000.0 / vm.fps : 41.67;
            // `upload` is included as of the step-8 measurement, and it was
            // MISSING before: on the CPU backend setFrame is a refcount bump and
            // a QImage view, so total genuinely was decode + convert + paint,
            // but on the d3d11 planar path there is a second copy between the
            // conversion and the draw that no term covered. `total` therefore
            // under-reported the shipping default. It reads 0 on cpu, so the
            // change is confined to the backend it was wrong for -- but a d3d11
            // `total` recorded before this commit is a smaller number for the
            // same work, and comparing across it would invent a regression.
            const double lastTotalMs = perf.lastDecodeMs + perf.lastConvertMs
                                     + perf.lastConvertWrapMs + perf.lastConvertAllocMs
                                     + drawPerf.lastUploadMs
                                     + drawPerf.lastPaintMs;

            // The SOURCE's shape, beside the encoded size it is not the same as
            // (spec phase 12). `sar` says whether square pixels were stated or
            // assumed, `rot` is the container's rotation metadata, and `dar` is
            // the composed display ratio the window is sized from -- printed
            // here rather than left to be recomputed, because the whole failure
            // this guards against is two places deriving it differently.
            QString shape = QStringLiteral(" | sar %1:%2%3 dar %4")
                                .arg(vm.sarNum)
                                .arg(vm.sarDen)
                                .arg(vm.sarStated ? QString() : QStringLiteral("*"))
                                .arg(QString::number(vm.displayAspect(), 'f', 4));
            if (vm.rotationDegrees != 0 || vm.rotationSnapped) {
                shape += QStringLiteral(" rot%1%2")
                             .arg(vm.rotationDegrees)
                             .arg(vm.rotationSnapped ? QStringLiteral("~") : QString());
            }
            const QString l1 = QString("%1 | %2x%3%12 | fps %4 | codec %5 | src %6 %7b -> dst %8 | F:%9 | open %10ms | first %11ms")
                .arg(QFileInfo(QString::fromStdString(currentMedia_->path)).fileName())
                .arg(vm.width)
                .arg(vm.height)
                .arg(QString::number(vm.fps, 'f', 3))
                .arg(vm.codecName)
                .arg(perf.srcPixelFormat)
                .arg(perf.srcBitDepth)
                .arg(perf.dstPixelFormat)
                .arg(st.currentFrame)
                .arg(QString::number(perf.openMs, 'f', 2))
                .arg(QString::number(perf.firstFrameMs, 'f', 2))
                .arg(shape);

            // Colour and resampling state: a picture that looks wrong is either
            // a matrix/range mismatch or a scaled presentation, and both are
            // otherwise invisible.
            //
            // The WINDOW SIZE is on this line because a scrub measurement is not
            // comparable across sessions without it, and that cost a session.
            // Section 17.4 recorded `stalls 2 of 394` on 4K H.264 reversals and
            // a later run of the same gesture on the same file read ~46 of ~380
            // -- neither capture said how big the window was. Cache depth is a
            // function of window size: previews convert to the size they will be
            // drawn at (`b5a56af`) and the cache is budgeted in bytes, so a
            // bigger window means bigger entries, fewer of them, a lower hit rate
            // and more seek-and-GOP-walk stalls. Measured on 4K H.264 reversals,
            // cpu, same build, same gesture:
            //
            //   window 900x854    cache 76/76   rev-hit 98.2%   stalls  46/375
            //   window 1300x1106  cache 41/41   rev-hit 97.1%   stalls  56/315
            //   window 1700x1354  cache 27/27   rev-hit 94.9%   stalls 140/228
            //   window 2100x1460  cache 22/22   rev-hit 92.0%   stalls 136/217
            //
            // A stall count quoted without the window it was taken in is not a
            // number anyone can check.
            const double hudDpr = viewer_->devicePixelRatioF();
            // `display WxH` is followed by how it was resampled, and `xN` is the
            // reduction's taps per axis. It belongs next to `display` because the
            // two together are the whole scaling story: the size it was drawn at
            // and how many source samples each output pixel came from. `x1` at a
            // real downscale is the undersampling case -- see RenderStats.
            QString resampleState =
                !drawPerf.lastDrawWasScaled
                    ? QStringLiteral("1:1")
                    : (drawPerf.lastDrawWasFiltered
                           ? QStringLiteral("filtered x%1").arg(drawPerf.reduceTaps)
                           : QStringLiteral("NEAREST"));
            // A view transform that silently fails to apply looks identical to
            // no transform from every other number, which is the same reason
            // `renderer` and `+overlay` are reported.
            if (const auto vt = viewer_->viewTransform(); !vt.isIdentity()) {
                resampleState += QStringLiteral(" view");
                if (vt.quarterTurns) resampleState += QStringLiteral(" rot%1").arg(vt.quarterTurns * 90);
                if (vt.flipH) resampleState += QStringLiteral(" flipH");
                if (vt.flipV) resampleState += QStringLiteral(" flipV");
            }
            // Spec phase 15, and reported for the same reason `view` is: a zoom
            // that silently failed to apply looks identical to no zoom from
            // every other number on this line. Absent while fitting, which is
            // the state every recorded measurement in this project was taken
            // in -- so a HUD capture with no `zoom` field is comparable to the
            // whole existing record, and one with it is not.
            if (!viewer_->isFitToWindow()) {
                resampleState += QStringLiteral(" zoom %1:1")
                                     .arg(QString::number(viewer_->currentScale(), 'f', 2));
                if (viewer_->canPan()) resampleState += QStringLiteral(" pannable");
            }
            const QString l0 = QString("color %1%2 %3 range | display %4x%5 %6 | win %7x%8 | renderer %9%10")
                .arg(perf.colorMatrix)
                .arg(perf.colorMatrixInferred ? "*" : "")
                .arg(perf.srcFullRange ? "full" : "limited")
                .arg(drawPerf.lastDrawSize.width())
                .arg(drawPerf.lastDrawSize.height())
                .arg(resampleState)
                // Device pixels, matching `display` above, so the two can be
                // compared without knowing the scale factor.
                .arg(static_cast<int>(std::lround(width() * hudDpr)))
                .arg(static_cast<int>(std::lround(height() * hudDpr)))
                // Which backend is actually presenting. A GPU path that quietly
                // fell back to cpu would otherwise be invisible.
                .arg(viewer_->rendererName())
                // And WHICH TRANSPORT is on screen. Named in both directions
                // rather than only when set: the floating overlay is the default
                // as of spec phase 6 and fades itself out, so neither state is
                // answerable from anything else on screen -- an absent panel
                // looks the same as a panel that was never enabled, and the
                // docked bar's absence is now the normal case rather than the
                // interesting one.
                .arg(viewer_->overlayEnabled() ? QStringLiteral(" +overlay")
                                               : QStringLiteral(" +bar"))
              // Spec phase 12's first experiment, on the line that already
              // carries `win` and `display`, because what it measures is what a
              // change to those two costs.
              //
              // `resize N` is resizeEvents, `chg` how many reached a real
              // preview-size change, `drop` how many cache ENTRIES those
              // discarded -- the last is the cost, because clearing an empty
              // cache is free and a count of clears would read as a thrash that
              // is not there. `sync` is the total and worst time inside
              // syncScrubPreviewSize itself. `wm` is sizing/enter/exit, counted
              // so the messages the aspect lock will be built on are proved to
              // arrive before anything depends on them.
              + QString(" | resize %1 chg %2 drop %3 sync %4/%5ms | wm %6/%7/%8 size %9")
                .arg(resizeEvents_)
                .arg(previewSizeChanges_)
                .arg(cacheEntriesDropped_)
                .arg(QString::number(syncPreviewMsTotal_, 'f', 1))
                .arg(QString::number(syncPreviewMsMax_, 'f', 2))
                .arg(wmSizing_)
                .arg(wmEnterSizeMove_)
                .arg(wmExitSizeMove_)
                .arg(wmSize_)
              // Section 20.4. `win` and `display` are both DEVICE pixels, so on
              // their own they cannot say whether a change came from a resize
              // or from a scale-factor change -- and that is precisely the
              // distinction this validation pass exists to make. `dpr` is the
              // scale factor actually in force, `scr` names the monitor the
              // window is on (so a monitor-to-monitor move is visible even
              // between two monitors at the SAME scale factor, where no
              // WM_DPICHANGED fires at all), and `dpiChg` counts the messages.
              //
              // `dpiChg 0` across a run that visibly crossed monitors is a
              // result, not a gap: it says the two monitors share a scale
              // factor.
              // `dpiChg` counts the MESSAGES and `reshape` counts what was done
              // about them, and they are separate fields because they are
              // separate claims. Before the section 20.4 fix the first read 1
              // and the second would have read 0 -- the message arrived, was
              // observed, and nothing recomputed. `reshape` lagging `dpiChg` is
              // also the correct reading for a move between two monitors that
              // share a scale factor.
              + QString(" | dpr %1 scr %2 dpiChg %3%4 reshape %5")
                .arg(QString::number(hudDpr, 'f', 2))
                .arg(windowHandle() && windowHandle()->screen()
                         ? windowHandle()->screen()->name()
                         : QStringLiteral("?"))
                .arg(wmDpiChanged_)
                .arg(wmDpiChanged_ > 0
                         ? QString(" (%1->%2)")
                               .arg(QString::number(lastDpiChangeFrom_, 'f', 2))
                               .arg(QString::number(lastDpiChangeTo_, 'f', 2))
                         : QString())
                .arg(dpiReshapes_);

            // `upload` is the CPU -> GPU transfer for one frame and `tex` is how
            // many GPU textures have been created since launch. Both read 0 on
            // the CPU backend, which is the honest answer rather than a gap.
            //
            // `tex` is cumulative on purpose: it is the measurement that decides
            // whether plan step 8 ("reuse textures and upload resources") has
            // anything left to do. A count that climbs through a run is churn; a
            // count that stops after the first frame of a source means the reuse
            // is already there and the remaining per-frame cost is the transfer
            // itself, which is a copy that has to happen somewhere.
            const QString l2 = QString("dec %1/%2 | sws %3/%4 | ctx %5/%6 | detach %7/%8 | alloc %9 | memcpy %10 | handoff %11 | upload %15/%16 tex %17 | draw %12 | total %13 | budget %14ms")
                .arg(QString::number(perf.lastDecodeMs, 'f', 2))
                .arg(QString::number(perf.avgDecodeMs, 'f', 2))
                .arg(QString::number(perf.lastSwsScaleMs, 'f', 2))
                .arg(QString::number(perf.avgSwsScaleMs, 'f', 2))
                .arg(QString::number(perf.lastCtxRebuildMs, 'f', 2))
                .arg(QString::number(perf.avgCtxRebuildMs, 'f', 2) + " [rb"
                     + QString::number(perf.swsSlotRebuilds) + "/slots"
                     + QString::number(perf.swsSlotsInUse) + "]")
                .arg(QString::number(perf.lastDetachMs, 'f', 2))
                .arg(QString::number(perf.avgDetachMs, 'f', 2))
                .arg(QString::number(perf.lastConvertAllocMs, 'f', 2))
                .arg(QString::number(perf.lastMemcpyMs, 'f', 2))
                .arg(QString::number(perf.lastHandoffMs, 'f', 2))
                .arg(QString::number(drawPerf.lastPaintMs, 'f', 2))
                .arg(QString::number(lastTotalMs, 'f', 2))
                .arg(QString::number(budgetMs, 'f', 2))
                .arg(QString::number(drawPerf.lastUploadMs, 'f', 2))
                .arg(QString::number(drawPerf.avgUploadMs, 'f', 2))
                .arg(drawPerf.textureCreates);

            const QString l3 = QString("cvt/req %1 | ctx-rebuilds %2 | shared %3 | sws %4 | %5 | rev-hit %6%% (%7/%8) | late %9 | walk %10f cache %11cv/%12ms | seek %13/%14 n=%15 | drain %16pk/%17f stale-blocked %18 recov %19 | thr %20")
                .arg(perf.lastConvertCalls)
                .arg(perf.lastCtxRebuilds)
                .arg(perf.lastImageWasShared ? "yes" : "no")
                .arg(perf.swsContextReused ? "reuse" : "rebuild")
                .arg(perf.alphaPlaneSkipped ? "a-skip" : "a-keep")
                .arg(QString::number(reverseHitRate, 'f', 1))
                .arg(perf.reverseCacheHits)
                .arg(perf.reverseCacheLookups)
                .arg(perf.lateFrames)
                .arg(perf.lastWalkFrames)
                .arg(perf.lastWalkCacheConverts)
                .arg(QString::number(perf.lastWalkCacheConvertMs, 'f', 2))
                .arg(QString::number(perf.lastSeekMs, 'f', 2))
                .arg(QString::number(perf.avgSeekMs, 'f', 2))
                .arg(perf.seekSamples)
                .arg(perf.drainPacketsSent)
                .arg(perf.drainFramesRecovered)
                .arg(perf.staleSuccessPrevented)
                .arg(perf.recoveredDecodeFailures)
                // Threading mode, constant for the life of a media open. It is
                // here because the walk-versus-seek limit is conditioned on it,
                // so a `walk`/`seek` figure cannot be read without it.
                .arg(perf.threadTypeIsFrame ? "frame" : "slice");

            // Presented rate from the wall clock: the only number that says
            // whether playback actually held real time.
            const double elapsedS = playbackRunElapsedS_;
            const bool rateValid = elapsedS > 0.5 && playbackFramesPresented_ > 0;
            const double presentedFps = rateValid
                ? static_cast<double>(playbackFramesPresented_) / elapsedS
                : 0.0;
            const double realTimePct = (rateValid && vm.fps > 0.0)
                ? 100.0 * presentedFps / vm.fps
                : 0.0;

            // Real-time frame dropping, made visible (owner requirement,
            // 2026-08-13). Reads `drop 0` on every source that keeps up, so a
            // non-zero value is itself the statement that the source could not
            // sustain its native rate -- and `media` beside it is the check that
            // the drop did its job: media time covered against wall time, which
            // must read ~100% whenever `real time` reads below it. The two
            // together are the whole contract: `real time` is how much PICTURE
            // arrived, `media` is whether the MOVIE stayed on the clock.
            const double mediaCoveredS = (vm.fps > 0.0)
                ? static_cast<double>(playbackFramesPresented_ + playbackDroppedFrames_) / vm.fps
                : 0.0;
            const double mediaPct = rateValid && elapsedS > 0.0
                ? 100.0 * mediaCoveredS / elapsedS
                : 0.0;
            const QString dropField =
                QString(" | drop %1 (ticks %2 max %3, media %4%%)")
                    .arg(playbackDroppedFrames_)
                    .arg(playbackDropTicks_)
                    .arg(maxDropRun_)
                    .arg(QString::number(mediaPct, 'f', 1));

            const QString l4 = rateValid
                ? QString("presented %1 / %2 fps (%3%% real time) | frames %4 | elapsed %5s%6")
                      .arg(QString::number(presentedFps, 'f', 2))
                      .arg(QString::number(vm.fps, 'f', 2))
                      .arg(QString::number(realTimePct, 'f', 1))
                      .arg(playbackFramesPresented_)
                      .arg(QString::number(elapsedS, 'f', 2))
                      .arg(dropField)
                : QString("presented -- / %1 fps | frames %2")
                      .arg(QString::number(vm.fps, 'f', 2))
                      .arg(playbackFramesPresented_);

            // `tick` is the delay the LAST wake was armed for, not a fixed
            // interval: GATE E re-arms per frame against an absolute deadline,
            // so at 24fps it alternates 41/42 and that alternation is the fix
            // working. A tick pinned at one value means the timeline is not
            // established -- no rational, or media that never started a run.
            // `jitter` is wake-to-wake interval against the true frame period,
            // which is what it always meant -- before GATE E the armed interval
            // WAS the period, so the figures stay comparable with section 23.4.
            // It is deliberately not measured against the armed interval any
            // more; see the computation for why that read 34ms on a schedule
            // that was within 1.8ms of its deadline.
            // `rephase` counts slots abandoned because a handler overran, which
            // is cost overrun (cause B) and is not something GATE E fixes.
            const QString l5 = QString("sched tick %1ms | jitter %2/%3/%4 (last/avg/max) | present-late %5/%6/%7 | rephase %8 | drift %9ms | ticks %10 | presents %11")
                .arg(schedulerIntervalMs_)
                .arg(QString::number(lastTickJitterMs_, 'f', 2))
                .arg(QString::number(avgTickJitterMs_, 'f', 2))
                .arg(QString::number(maxTickJitterMs_, 'f', 2))
                .arg(QString::number(lastPresentLatencyMs_, 'f', 2))
                .arg(QString::number(avgPresentLatencyMs_, 'f', 2))
                .arg(QString::number(maxPresentLatencyMs_, 'f', 2))
                .arg(presentRephaseCount_)
                .arg(QString::number(lastDriftMs_, 'f', 1))
                .arg(schedulerTicks_)
                .arg(presentSamples_);

            // Cadence distribution. The rate above averages and reads 98-99%
            // whether the fault is the tick beat or per-frame cost overrun, so
            // this is the line that says which. Percentiles come from a sorted
            // copy -- a 10s run is ~240 samples, so exact beats approximate.
            QString l5b = QStringLiteral("cadence | no samples yet");
            if (!cadenceGapsMs_.empty()) {
                std::vector<double> g = cadenceGapsMs_;
                std::sort(g.begin(), g.end());
                const auto pct = [&g](double p) {
                    const std::size_t i = std::min(g.size() - 1,
                        static_cast<std::size_t>(p * static_cast<double>(g.size() - 1) + 0.5));
                    return g[i];
                };
                const double budget = tickFrameDurationMs_ > 0.0 ? tickFrameDurationMs_ : 41.667;
                // Buckets as multiples of the frame budget. A regular beat piles
                // up in [1.5,2.5) and nowhere else; ragged overrun smears.
                int b[5] = {0, 0, 0, 0, 0};
                for (double v : g) {
                    const double r = v / budget;
                    if (r < 0.9) ++b[0];
                    else if (r < 1.1) ++b[1];
                    else if (r < 1.5) ++b[2];
                    else if (r < 2.5) ++b[3];
                    else ++b[4];
                }
                // Spacing between long frames: regular means a beat, scattered
                // means overrun. Reported as min/median/max so one outlier
                // cannot make a ragged run look periodic.
                QString spacing = QStringLiteral("--");
                if (cadenceLongAt_.size() >= 2) {
                    std::vector<long long> d;
                    d.reserve(cadenceLongAt_.size() - 1);
                    for (std::size_t i = 1; i < cadenceLongAt_.size(); ++i) {
                        d.push_back(cadenceLongAt_[i] - cadenceLongAt_[i - 1]);
                    }
                    std::sort(d.begin(), d.end());
                    spacing = QString("%1/%2/%3").arg(d.front()).arg(d[d.size() / 2]).arg(d.back());
                }
                l5b = QString("cadence n%1 | p50 %2 p95 %3 p99 %4 max %5 | <0.9x %6 ~1x %7 1.1-1.5x %8 "
                              "1.5-2.5x %9 >2.5x %10 | long-gap min/med/max %11 | handler>budget %12 of %13 (max %14)")
                    .arg(g.size())
                    .arg(QString::number(pct(0.50), 'f', 1))
                    .arg(QString::number(pct(0.95), 'f', 1))
                    .arg(QString::number(pct(0.99), 'f', 1))
                    .arg(QString::number(g.back(), 'f', 1))
                    .arg(b[0]).arg(b[1]).arg(b[2]).arg(b[3]).arg(b[4])
                    .arg(spacing)
                    .arg(handlerOverBudget_)
                    .arg(handlerSamples_)
                    .arg(QString::number(maxHandlerMs_, 'f', 1));
            }

            // Span-based rate: N presented frames cover N-1 intervals, so this
            // excludes both startup before frame 1 and any end-of-stream hold.
            const double spanS = (firstPresentNs_ >= 0 && lastPresentNs_ > firstPresentNs_)
                ? static_cast<double>(lastPresentNs_ - firstPresentNs_) / 1'000'000'000.0
                : 0.0;
            const double spanFps = (spanS > 0.0 && playbackFramesPresented_ > 1)
                ? static_cast<double>(playbackFramesPresented_ - 1) / spanS
                : 0.0;

            const QString l6 = QString("period %1/%2/%3 | handler %4/%5 | outside %6/%7 | paint %8/%9 tot %10 draw %11 upd->paint %12 | paints %13/%14 | span %15s span-fps %16")
                .arg(QString::number(lastPeriodMs_, 'f', 2))
                .arg(QString::number(avgPeriodMs_, 'f', 2))
                .arg(QString::number(maxPeriodMs_, 'f', 2))
                .arg(QString::number(lastHandlerMs_, 'f', 2))
                .arg(QString::number(avgHandlerMs_, 'f', 2))
                .arg(QString::number(lastOutsideMs_, 'f', 2))
                .arg(QString::number(avgOutsideMs_, 'f', 2))
                .arg(QString::number(drawPerf.lastPaintMs, 'f', 2))
                .arg(QString::number(drawPerf.avgPaintMs, 'f', 2))
                .arg(QString::number(drawPerf.avgPaintTotalMs, 'f', 2))
                .arg(QString::number(drawPerf.avgDrawImageMs, 'f', 2))
                .arg(QString::number(drawPerf.avgUpdateToPaintMs, 'f', 2))
                .arg(drawPerf.paintCount)
                .arg(drawPerf.updateCount)
                .arg(QString::number(spanS, 'f', 2))
                .arg(QString::number(spanFps, 'f', 2));

            // Storage + I/O. The whole point of splitting playback from seek is
            // that averaging them cannot answer whether ordinary forward
            // playback is read-starved.
            const auto& ioPlay = hudIo_[static_cast<int>(trace::core::IoPhase::Playback)];
            const auto& ioSeek = hudIo_[static_cast<int>(trace::core::IoPhase::Seek)];
            const auto& ioOpen = hudIo_[static_cast<int>(trace::core::IoPhase::Open)];

            const QString lio1 = QString("src %1 | %2 | %3 MB | %4 Mbps | iobuf %5 KB")
                .arg(perf.sourceStorage)
                .arg(perf.sourceVolume)
                .arg(QString::number(perf.sourceBytes / (1024.0 * 1024.0), 'f', 1))
                .arg(QString::number(perf.sourceBitrateMbps, 'f', 1))
                .arg(perf.ioBufferBytes / 1024)
              + QString(" | open: classify %1%2 + fileopen %3 + demux %4 + streaminfo %5 ms")
                .arg(QString::number(perf.openClassifyMs, 'f', 1))
                .arg(perf.classifyCached ? "(cached)" : "")
                .arg(QString::number(perf.openFileMs, 'f', 1))
                .arg(QString::number(perf.openDemuxMs, 'f', 1))
                .arg(QString::number(perf.openStreamInfoMs, 'f', 1))
              // The Share gate (spec phase 8), on the line that already reports
              // the storage classification it is built from -- so the necessary
              // condition and the verdict it feeds are read together and a
              // disagreement between them is visible rather than inferred from
              // whether a menu item looked grey in a screenshot.
              + QString(" | share path %1 explorer %2 lucid %3")
                .arg(shareAvailabilityName(shareState_.copyPath))
                .arg(shareAvailabilityName(shareState_.showInExplorer))
                .arg(shareAvailabilityName(shareState_.lucidLink))
              // Open Recent (spec phase 11), on the same line for the same
              // reason the Share gate is: neither is answerable from a
              // screenshot of the window. How many entries are held, and WHICH
              // settings home they are held in -- portable trace.ini beside the
              // executable, or the per-user file -- where nothing on screen
              // says which one won.
              + QString(" | recent %1/%2 %3")
                .arg(recentFiles_.paths().size())
                .arg(trace::app::RecentFiles::kMaxEntries)
                .arg(trace::app::settingsModeName());

            const QString lprobe = QString("probe | limit %1 KB / %2 ms | rd %3 | %4 KB | seek %5 | streams %6 | fps %7 | dur %8s | frames %9")
                .arg(perf.probeSizeLimit / 1024)
                .arg(perf.analyzeDurationUs / 1000)
                .arg(perf.probeReads)
                .arg(QString::number(perf.probeBytes / 1024.0, 'f', 1))
                .arg(perf.probeSeeks)
                .arg(perf.streamCount)
                .arg(QString::number(vm.fps, 'f', 6))
                .arg(QString::number(vm.durationSeconds, 'f', 3))
                .arg(vm.frameCount);

            // Responsiveness, which is what Pass 1 is judged on. `uiblock` is
            // the longest stretch the UI thread went without servicing events
            // during a read -- not how long the read took.
            const QString lresp = QString("resp | uiblock play %1ms seek %2ms open %3ms | buffering %4 ev %5ms | waiting %6ms | gen %7 drop %8")
                .arg(QString::number(ioPlay.callerBlockMaxMs, 'f', 1))
                .arg(QString::number(ioSeek.callerBlockMaxMs, 'f', 1))
                .arg(QString::number(ioOpen.callerBlockMaxMs, 'f', 1))
                .arg(bufferingEvents_)
                .arg(QString::number(bufferingMsTotal_, 'f', 0))
                .arg(QString::number(maxStorageWaitMs_, 'f', 0))
                // `gen` counts how often the target moved; `drop` counts results
                // that were actually discarded for being stale. On local media
                // gen climbs with every drag update and drop stays 0.
                .arg(requestGeneration_)
                .arg(supersededResults_);

            auto ioLine = [](const char* tag, const trace::core::IoPhaseStats& s) {
                return QString("io %1 | rd %2 | avg %3 KB (min %4 max %5) | seq %6%% "
                               "| seek %7 | lat %8/%9ms | %10 Mbps | stall %11 (%12ms)")
                    .arg(tag)
                    .arg(s.reads)
                    .arg(QString::number(s.avgReadBytes() / 1024.0, 'f', 1))
                    .arg(s.minReadBytes)
                    .arg(s.maxReadBytes)
                    .arg(QString::number(s.sequentialFraction() * 100.0, 'f', 1))
                    .arg(s.seeks)
                    .arg(QString::number(s.avgLatencyMs(), 'f', 3))
                    .arg(QString::number(s.latencyMaxMs, 'f', 1))
                    .arg(QString::number(s.readMbps(), 'f', 0))
                    .arg(s.stalls)
                    .arg(QString::number(s.stallMsTotal, 'f', 0));
            };

            const QString lio2 = ioLine("open", ioOpen);
            const QString lio3 = ioLine("play", ioPlay);
            const QString lio4 = ioLine("seek", ioSeek);

            // Audio line. `sync` is the number that decides whether audio-master
            // is working: picture position minus audio clock, in ms. Under
            // about +/-20ms nobody can see it; a number that grows without
            // bound means the clock is not actually driving.
            const auto audioStats = audio_.stats();
            const QString l9 = audioStats.disabledByEnv
                ? QStringLiteral("audio DISABLED (TRACE_NO_AUDIO) - wall-clock control test")
                : !audioStats.available
                ? QStringLiteral("audio none")
                : QString("audio %1 %2Hz %3ch%4 | %5 | sync %6ms (max %7) | buf %8ms | under %9 | rep %10 skip %11")
                    .arg(audioStats.codecName)
                    .arg(audioStats.sampleRate)
                    .arg(audioStats.channels)
                    .arg(audioStats.muted ? " MUTED" : "")
                    .arg(audioClockStalled_ ? "STALLED"
                         : audioClockPriming_ ? "PRIMING"
                         : audioDriving_ ? "MASTER" : "idle")
                    .arg(QString::number(lastAvSyncMs_, 'f', 1))
                    .arg(QString::number(maxAvSyncMs_, 'f', 1))
                    .arg(QString::number(audioStats.bufferedMs, 'f', 0))
                    .arg(audioStats.underruns)
                    .arg(audioRepeatedFrames_)
                    .arg(audioSkippedFrames_)
                  + QString(" | proc %1ms sinkbuf %2 free %3 state %4 clk %5s")
                    .arg(audioStats.processedUSecs / 1000)
                    .arg(audioStats.sinkBufferBytes)
                    .arg(audioStats.sinkFreeBytes)
                    .arg(audioStats.sinkState)
                    .arg(QString::number(audioStats.clockSeconds, 'f', 3));

            // Buffer geometry and clock-loop health. The startup churn was
            // caused by the device buffer being twice what the clock constants
            // assumed, so requested-vs-actual, the ring invariant, and the
            // silence padding all have to be readable rather than inferred.
            // `clk-upd` must read 1/1: anything higher means telemetry is
            // stepping the control loop and the HUD is changing playback.
            const QString l10 = !audioStats.available
                ? QString()
                : QString("audiobuf req %1 KB / got %2 KB (%3ms) | ring %4 KB (%5ms, %6x) | fill %7ms | silence %8 B | lat %9ms | snap %10ms x%11 | clk-upd %12/%13")
                    .arg(audioStats.sinkBufferRequestedBytes / 1024)
                    .arg(audioStats.sinkBufferBytes / 1024)
                    .arg(QString::number(audioStats.deviceBufferMs, 'f', 0))
                    .arg(audioStats.ringCapacityBytes / 1024)
                    .arg(QString::number(audioStats.ringCapacityMs, 'f', 0))
                    .arg(QString::number(
                        audioStats.sinkBufferBytes > 0
                            ? static_cast<double>(audioStats.ringCapacityBytes)
                                  / static_cast<double>(audioStats.sinkBufferBytes)
                            : 0.0, 'f', 2))
                    .arg(QString::number(audioStats.startupFillMs, 'f', 1))
                    .arg(audioStats.silenceBytes)
                    .arg(QString::number(audioStats.smoothedLatencyMs, 'f', 1))
                    .arg(QString::number(audioStats.snapThresholdMs, 'f', 0))
                    .arg(audioStats.clockSnaps)
                    .arg(lastClockUpdatesPerTick_)
                    .arg(maxClockUpdatesPerTick_);

            // Capacity is the count that fits at the size currently stored, so
            // it rises as a 4K drag fills the cache with half-res previews.
            // The MB pair is the real rule; the count is derived from it.
            const QString l8 = QString("cache FIFO | %1/%2 (%3/%9 MB) | hit %4%% (%5/%6) | ins %7 evict %8")
                .arg(perf.cacheOccupancy)
                .arg(perf.cacheCapacity)
                .arg(QString::number(static_cast<double>(perf.cacheBytes) / (1024.0 * 1024.0), 'f', 1))
                .arg(QString::number(reverseHitRate, 'f', 1))
                .arg(perf.reverseCacheHits)
                .arg(perf.reverseCacheLookups)
                .arg(perf.cacheInserts)
                .arg(perf.cacheEvictions)
                .arg(QString::number(static_cast<double>(perf.cacheBudgetBytes) / (1024.0 * 1024.0), 'f', 0));

            // `shown`/`delta` are measured, not asserted. They used to be
            // assigned -- Scrub wrote the requested index onto whatever
            // keyframe the seek landed on, so this line read `exact | delta 0`
            // while displaying a frame most of a GOP away.
            //
            // They are now read off the frame that was actually delivered
            // rather than from the decoder's per-decode perf fields. Those went
            // stale on a cache hit, because the hit path returns before they
            // are written -- so a drag running on cache hits reported whatever
            // the last real decode had left behind. The frame carries its own
            // index, so a hit reports as honestly as a decode.
            const QString l7 = QString("scrub %1 | target %2 | shown %3 | delta %4 | walk %5f | dst %6 | shuttle %7ms/f lag %8f")
                .arg(perf.previewApproximate ? "APPROX" : "exact")
                .arg(lastRequestedFrame_)
                .arg(lastDeliveredFrame_)
                .arg(lastRequestedFrame_ >= 0 && lastDeliveredFrame_ >= 0
                         ? lastRequestedFrame_ - lastDeliveredFrame_ : 0)
                .arg(perf.lastWalkFrames)
                .arg(perf.dstPixelFormat)
                .arg(QString::number(scrubWalkPerFrameMs_, 'f', 2))
                // How far the picture is trailing the pointer mid-drag. Non-zero
                // is expected and is the eased catch-up; it should fall to 0
                // shortly after the pointer stops.
                .arg(pendingScrubFrame_ >= 0 && activeScrubFrame_ >= 0
                         ? std::llabs(pendingScrubFrame_ - activeScrubFrame_) : 0LL);

            // Smoothness rather than throughput: the gap between consecutive
            // paints, how many landed too fast for the display to have shown
            // the previous one, and how many were long enough to notice. A drag
            // can be perfect on `shuttle`/`lag` and bad here.
            //
            // TWO thresholds, and the reason is that the first one moves.
            // `stalls` is `gap > 2 x refresh`, so it reads 8.3ms on this
            // 239.999Hz panel and 33.3ms on the 60Hz mode the same box was
            // observed in -- a factor of four, from the monitor. It is printed
            // WITH its threshold now, because a count in a unit the display
            // chose is not comparable to one recorded in another session and
            // nothing said which unit was in force. `hitch` is absolute and is
            // the one to quote.
            const double gapAvg = scrubPaintGapSamples_ > 0
                ? scrubPaintGapSumMs_ / static_cast<double>(scrubPaintGapSamples_) : 0.0;
            const double wastedPct = scrubPaintGapSamples_ > 0
                ? 100.0 * static_cast<double>(scrubPaintsWasted_)
                      / static_cast<double>(scrubPaintGapSamples_) : 0.0;
            const QScreen* smoothScr = screen();
            const double smoothHz = (smoothScr && smoothScr->refreshRate() > 1.0)
                ? smoothScr->refreshRate() : 60.0;
            const QString l7b = QString("smooth | gap %1/%2/%3ms (last/avg/max) | wasted %4%% (%5) | stalls %6 of %7 (>%8ms) | hitch %9 (>%10ms)")
                .arg(QString::number(scrubPaintGapLastMs_, 'f', 1))
                .arg(QString::number(gapAvg, 'f', 1))
                .arg(QString::number(scrubPaintGapMaxMs_, 'f', 1))
                .arg(QString::number(wastedPct, 'f', 0))
                .arg(scrubPaintsWasted_)
                .arg(scrubPaintStalls_)
                .arg(scrubPaintGapSamples_)
                .arg(QString::number(2000.0 / smoothHz, 'f', 1))
                .arg(scrubPaintHitches_)
                .arg(QString::number(kScrubHitchMs, 'f', 0));

            // Responsiveness of the thread rather than of the picture. `ui gap`
            // is measured by a 1ms timer that only fires when the event loop is
            // running, so its worst interval is how long the window could not
            // deliver a mouse move or repaint -- which is what the slider handle
            // trailing the pointer actually is. `release` is the one blocking
            // decode that is meant to be there.
            const double uiGapAvg = uiServiceSamples_ > 0
                ? uiServiceGapSumMs_ / static_cast<double>(uiServiceSamples_) : 0.0;
            // `landing` is where the exact landing went. `async/sync` says which
            // path a click, a release and a step took, so a build that silently
            // fell back -- no video, storage busy, TRACE_ASYNC_LANDING=0 -- is
            // visible rather than inferred; `sup` counts landings superseded
            // before they arrived, which is the user moving on and not an error.
            // The two latency figures are the same quantity as `release`: post
            // to on-screen. NONE of them is UI-thread occupancy -- that is
            // `gap max`, immediately to the left, and it is the field this
            // change is judged on.
            const QString l7c = QString("ui | gap %1/%2ms (avg/max) | over %3ms: %4 of %5 | release %6ms"
                                        " | landing %7 async %8 sync %9 sup %10 (%11ms max %12ms)")
                .arg(QString::number(uiGapAvg, 'f', 2))
                .arg(QString::number(uiServiceGapMaxMs_, 'f', 1))
                .arg(QString::number(kUiServiceGapMs, 'f', 0))
                .arg(uiServiceGapsOver_)
                .arg(uiServiceSamples_)
                .arg(QString::number(scrubReleaseLatencyMs_, 'f', 1))
                .arg(!asyncLandingEnabled() ? "OFF"
                                            : (landingPending_ ? "PENDING" : "idle"))
                .arg(landingsAsync_)
                .arg(landingsSync_)
                .arg(landingsSuperseded_)
                .arg(QString::number(landingLatencyMs_, 'f', 1))
                .arg(QString::number(landingLatencyMaxMs_, 'f', 1));

            // The worker. `cancel` is the number that decides whether
            // cooperative cancellation is worth having: a 100ms walk that is
            // eventually discarded is still bad if the pointer moved 90ms ago.
            // `ckpt` is the decoder's contribution to it -- the longest gap
            // between consecutive chances to notice -- so a bad `cancel` can
            // be attributed to the checkpoint granularity or to the wait
            // itself rather than guessed at.
            // The lag model. `ptr` is what the hand asked for and `dec` is what
            // the decoder supplied, both in source frames per second: their
            // ratio is the whole of it. A deficit there cannot be prefetched
            // away -- no scheduling makes a decoder faster -- whereas lag with
            // dec >= ptr means the work is going to the wrong frames.
            // `p2p` is the same story in the unit the user feels: how long ago
            // their hand was where the picture now is.
            const double lagRatio = scrubPointerFps_ > 0.1
                ? scrubDecodeFps_ / scrubPointerFps_ : 0.0;
            // Sampling state. `stride 1` is the shipped every-frame behaviour;
            // anything above it means the decoder could not keep up and the
            // preview is showing every Nth frame instead of falling behind.
            // `skipped` is only ever frames the ACTIVE DRAG stepped over -- the
            // landing, stepping and playback never sample.
            // `ra-walk` is the gate: the running mean of frames a request had
            // to walk to reach its target. Below 1 random access is cheap and
            // sampling is allowed; above it a strided step would leave the
            // region the decoder already opened and pay for a new one.
            // The reverse pipeline. `q` is how many decoded frames are waiting
            // ahead of the presentation point and is the number that says
            // whether the queue is doing its job: a queue that sits at 0 is a
            // pipeline that never got ahead, and `starve` counts the slots that
            // had no frame to show. A run with starve 0 presented every slot it
            // was asked for.
            // `land` is the UI-THREAD cost of endShuttleRun's landing branch --
            // a synchronous Step decode, which on a long-GOP file is a seek plus
            // a GOP walk. It is reported separately because it happens BEFORE
            // beginPlaybackTimeline() resets the run counters, so every other
            // instrument in this HUD measures it as free.
            // `loop` is on this line because it decides what the END of a run
            // does, and because a wrap RE-ESTABLISHES the playback timeline and
            // therefore zeroes every cadence counter beside it. A run that
            // wrapped is reporting one lap, and `wraps` is what says so -- a
            // cadence figure quoted from a looping run without it is the
            // stalls-versus-refresh mistake in a new costume.
            const QString l7g = QString("loop %16 wraps %17 | shuttle | %1 %9 | stride %2 adv %10 | %11 gop %12 | q %3/%4 (max %5) | starve %6 | next %7 | last %8 | land %13 (%14ms max %15ms)")
                .arg(!shuttleAsyncEnabled() ? "OFF" : shuttleRunActive_ ? "RUN" : "idle")
                .arg(shuttleStride_)
                .arg(static_cast<long long>(shuttleQueue_.size()))
                .arg(kShuttleQueueDepth)
                .arg(shuttleQueueMaxSeen_)
                .arg(shuttleStarves_)
                .arg(shuttleNextTarget_)
                .arg(shuttleLastPresented_)
                .arg(shuttleDir_ > 0 ? "FWD" : "REV")
                .arg(shuttleAdvance_)
                .arg(shuttleSnapping_ ? "SNAP" : "walk")
                .arg(shuttleGop_)
                .arg(shuttleLandCount_)
                .arg(shuttleLandLastMs_, 0, 'f', 1)
                .arg(shuttleLandMaxMs_, 0, 'f', 1)
                .arg(loopEnabled_ ? "ON" : "off")
                .arg(loopWraps_);

            // Checkpoint 2 stage one. The terms are separate because they are
            // separate claims and the owner asked for them that way.
            //
            // `wait` MUST READ 0. It is time the TICK spent inside the queue,
            // and the whole design is that a starve holds rather than waits --
            // a non-zero value means something is blocking on the pipeline.
            //
            // `starve` is what justifies the depth: the number is to come from
            // the measured starvation count rather than be chosen, so a depth
            // sweep reports this at each setting.
            //
            // `drop` is entries the audio clock or the real-time drop moved
            // past, and `reseed` is the queue re-anchoring after the target
            // jumped clear of it. Both are the queue obeying the clock rather
            // than the queue being wrong, which is why neither is an error.
            //
            // DO NOT INFER OVERLAP FROM THE FRAME RATE. The check that overlap
            // is real is `handler` collapsing to upload+paint while `dec` and
            // `sws` stay where they are, with `outside` rising to absorb the
            // difference. If `handler` falls and `dec` falls with it, the run
            // decoded fewer frames rather than overlapping them.
            const QString l7h = QString("pq %1 %2/%3 (max %4) | starve %5 | drop %6 | reseed %7 | wait %8ms | posted %9 | %10/%11 MB")
                .arg(playbackQueueRequestedDepth() <= 0 ? "OFF"
                     : playbackPrefetchActive_ ? "ON" : "idle")
                .arg(static_cast<long long>(playbackQueue_.size()))
                .arg(playbackQueueDepthForMedia())
                .arg(pqMaxDepth_)
                .arg(pqStarves_)
                .arg(pqAheadDrops_)
                .arg(pqReseeds_)
                .arg(QString::number(pqWaitMaxMs_, 'f', 2))
                .arg(pqPosted_)
                .arg(QString::number(static_cast<double>(pqBytes_) / (1024.0 * 1024.0), 'f', 0))
                .arg(QString::number(static_cast<double>(pqPeakBytes_) / (1024.0 * 1024.0), 'f', 0));

            // `batch` sits beside `stride` because they are the two ways one
            // request can cover more than one frame and they are mutually
            // exclusive by construction -- reading them together is what says
            // which mechanism is in force. `cap` is what was allowed, `last`
            // what the most recent request actually produced and `max` the
            // high-water mark: on heavy media the budget cuts a batch of 4 to
            // 1, and `cap 4 last 1` is how that is told apart from a rule that
            // never ran.
            const QString l7f = QString("sample %1 | stride %2 | batch cap %8 last %9 max %10 | skipped %3 over %4 steps | ctrl ptr %5 f/s cap %6 f/s | ra-walk %7f/seek")
                .arg(!scrubSamplingEnabled() ? "OFF"
                     : !videoDecoder_.metadata().intraOnly ? "GATED"
                     : scrubStride_ > 1 ? "ON" : "idle")
                .arg(scrubStride_)
                .arg(scrubFramesSkipped_)
                .arg(scrubSampledSteps_)
                .arg(QString::number(ctrlPointerFps_, 'f', 1))
                .arg(QString::number(scrubDecodeFps_, 'f', 1))
                .arg(QString::number(mediaWalkPerSeek(), 'f', 2))
                .arg(scrubBatchCap())
                .arg(scrubWorker_.lastBatchDecoded())
                .arg(scrubWorker_.maxBatchDecoded());

            const QString l7e = QString("lag | dir %10 rev %11 | ptr %1 f/s | dec %2 f/s | supply %3%% | behind %4/%5f | p2p %6/%7ms | walk max %8f | seeks %9")
                .arg(QString::number(scrubPointerFps_, 'f', 1))
                .arg(QString::number(scrubDecodeFps_, 'f', 1))
                .arg(QString::number(lagRatio * 100.0, 'f', 0))
                .arg(scrubLagLastFrames_)
                .arg(scrubLagMaxFrames_)
                .arg(QString::number(scrubPointerToPreviewMs_, 'f', 0))
                .arg(QString::number(scrubPointerToPreviewMaxMs_, 'f', 0))
                .arg(scrubWalkMaxFrames_)
                .arg(perf.seekSamples - scrubSeeksAtGestureStart_)
                .arg(scrubDirection_ > 0 ? "FWD" : scrubDirection_ < 0 ? "REV" : "--")
                .arg(scrubReversals_);

            const QString l7d = QString("worker %1 | posted %2 coalesced %3 | abandoned %4 stale %5 | cancel %6/%7ms | ckpt %8ms")
                .arg(asyncScrubEnabled() ? (decoderLeased_ ? "LEASED" : "async") : "OFF")
                .arg(scrubWorker_.requestsPosted())
                .arg(scrubWorker_.requestsCoalesced())
                .arg(perf.walksAbandoned)
                .arg(scrubWorker_.resultsStale())
                .arg(QString::number(scrubWorker_.lastCancelWaitMs(), 'f', 2))
                .arg(QString::number(scrubWorker_.maxCancelWaitMs(), 'f', 2))
                .arg(QString::number(perf.maxCheckpointGapMs, 'f', 2));

            line = l1 + "\n" + l0 + "\n" + l2 + "\n" + l3 + "\n" + l4 + "\n" + l5 + "\n" + l5b + "\n" + l6
                 + "\n" + l7 + "\n" + l7b + "\n" + l7c + "\n" + l7d + "\n" + l7e + "\n" + l7f + "\n" + l7g + "\n" + l7h + "\n" + l8 + "\n" + l9
                 + (l10.isEmpty() ? QString() : "\n" + l10)
                 + "\n" + lio1 + "\n" + lprobe + "\n" + lresp + "\n" + lio2 + "\n" + lio3 + "\n" + lio4;
        } else if (currentMedia_->kind == MediaKind::ImageSequence && currentMedia_->sequence.has_value()) {
            const auto& seq = *currentMedia_->sequence;
            // ZERO-BASED, and against the last valid INDEX rather than the
            // count -- which is what the video line has always printed and what
            // these two did not (spec §2 item 8). `Elapsed:` rather than
            // `Timecode:` because an image sequence has no container timecode
            // at all, so calling this one was the clearest instance of the thing
            // the spec forbids.
            line = QString("Sequence | %1 | %2x%3 ch:%4 | Frame: %5/%6 | Seconds: %7 | Elapsed: %8")
                .arg(QString::fromStdString(seq.pattern))
                .arg(currentImage_.has_value() ? currentImage_->width : 0)
                .arg(currentImage_.has_value() ? currentImage_->height : 0)
                .arg(currentImage_.has_value() ? currentImage_->channels : 0)
                .arg(st.currentFrame)
                .arg(seq.frames.empty() ? 0 : seq.frames.size() - 1)
                .arg(trace::core::TimeFormat::formatSeconds(sec))
                .arg(elapsed);
        } else if (currentImage_.has_value()) {
            const auto& im = *currentImage_;
            line = QString("Still | %1 | %2x%3 ch:%4 | Frame: %5/0 | Seconds: %6 | Elapsed: %7")
                .arg(im.fileName)
                .arg(im.width)
                .arg(im.height)
                .arg(im.channels)
                .arg(st.currentFrame)
                .arg(trace::core::TimeFormat::formatSeconds(sec))
                .arg(elapsed);
        }
    }

    overlay_->setTransport(mode, st.currentFrame, st.speed, action.isEmpty() ? primaryReadout : action + " | " + primaryReadout);
    overlay_->setHudLine(line);
    syncTransportBar();
}

// Dispatch only. Every key Trace answers to is declared in setupShortcuts(),
// and this walks that one table rather than a switch that only it could read.
//
// The rows carrying a QAction never reach here at all -- Qt runs an action's
// shortcut before the key event is delivered to the window -- so they are in the
// table as documentation for spec phase 13 and are skipped by the dispatcher.
void MainWindow::keyPressEvent(QKeyEvent* event) {
    if (shortcuts_.dispatch(event)) {
        // "Relevant keyboard input reveals it" (spec phase 6). The table IS the
        // definition of relevant: every key in it is a transport, stepping,
        // shuttle or readout command, and a key it does not own is by
        // construction not one. Revealing after the handler, so a shuttle press
        // brings the panel back already showing its new rate.
        if (viewer_) viewer_->revealOverlay();
        return;
    }
    QMainWindow::keyPressEvent(event);
}

void MainWindow::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void MainWindow::dropEvent(QDropEvent* event) {
    const auto urls = event->mimeData()->urls();
    if (urls.isEmpty()) return;
    const QString path = urls.first().toLocalFile();
    if (!path.isEmpty()) openPath(path);
}

trace::core::VideoFrameSource* MainWindow::videoFrameSource() {
    return dynamic_cast<trace::core::VideoFrameSource*>(frameSource_.get());
}

void MainWindow::prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode mode, int direction, bool clearQueue) {
    // Sets decoder state, so it needs the decoder back. Never reached from the
    // async drag path, which passes its mode and direction in the request.
    reclaimDecoder();
    auto* videoSource = videoFrameSource();
    if (!videoSource) return;
    videoSource->setRequestMode(mode);
    videoSource->setPlaybackDirection(direction);
    if (clearQueue) {
        videoSource->clearPlaybackQueue();
    }
}

} // namespace trace::app

