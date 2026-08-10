#include "app/MainWindow.h"

#include <QScreen>

#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMimeData>
#include <QResizeEvent>
#include <QStatusBar>

#include <QCoreApplication>
#include <QEventLoop>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>
#include <QtGlobal>

#include <algorithm>
#include <cmath>

#include "ui/OverlaySpike.h"
#include "ui/ViewerWidget.h"
#include "ui/TransportOverlay.h"
#include "ui/TransportBar.h"
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
    reclaimDecoder();
    scrubWorker_.stop();
}

MainWindow::MainWindow() {
    setWindowTitle("Trace");
    setAcceptDrops(true);
    setupUi();
    setupMenus();
    setupTransportControls();

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
        const double speed = std::max(1.0, std::abs(playbackState.speed));
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

            // One frame per presentation, never skipped: ordering over rate.
            steps = 1;
            playbackAccumulatorMs_ -= frameDurationMs;
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
            constexpr long long kMaxCatchUpFrames = 3;
            const long long advance = std::min(delta, kMaxCatchUpFrames);
            if (advance > 1) audioSkippedFrames_ += advance - 1;
            unclampedTarget = beforeFrame + advance;
        }

        const long long minFrame = 0;
        const long long maxFrame = playbackState.maxFrame >= 0 ? playbackState.maxFrame : beforeFrame;
        const long long targetFrame = std::clamp(unclampedTarget, minFrame, maxFrame);
        playback_.setCurrentFrame(targetFrame);

        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, direction);
        QString error;
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Playback)) {
            playback_.setCurrentFrame(beforeFrame);
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
            if (!error.isEmpty()) statusBar()->showMessage(error, 2000);
        } else {
            ++playbackFramesPresented_;
            playbackRunElapsedS_ = static_cast<double>(playbackRateClock_.elapsed()) / 1000.0;
            // First/last present span: N presented frames cover N-1 intervals,
            // so rate from this span is the honest steady-state figure.
            const qint64 nowNs = sessionClock_.isValid() ? sessionClock_.nsecsElapsed() : 0;
            if (firstPresentNs_ < 0) firstPresentNs_ = nowNs;

            // Cadence distribution, not just its mean. The presented rate reads
            // 98-99% under two completely different faults and therefore cannot
            // tell them apart, which is why a file can measure 99% and still
            // visibly stutter:
            //
            //   the integer tick beat -- floor(1000/fps) is 41ms against a
            //   41.667ms frame, so the accumulator falls 0.667ms short each
            //   frame and roughly every 62nd frame needs two ticks. That is one
            //   doubled (~83ms) frame every ~2.6s, REGULARLY spaced, on every
            //   file. It is a presentation-clock problem and GATE E owns it.
            //
            //   per-frame cost overrun -- playback decodes synchronously with no
            //   read-ahead, so a frame that misses the budget is late at once and
            //   nothing absorbs it. Ragged, irregular, and specific to whichever
            //   file is expensive. A presentation clock supplies phase, not
            //   headroom, so GATE E does NOT fix this one.
            //
            // The spacing between long frames is what separates them: a beat is
            // regular, an overrun is not. Sampling intervals BETWEEN PRESENTS
            // rather than between ticks is deliberate -- a held frame produces no
            // present, so the doubled interval only appears here.
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
            // Clock drift: ideal media time for the frames presented so far
            // versus wall clock. Positive = ahead of real time, negative =
            // behind. A scheduler holding rate keeps this flat near zero.
            lastDriftMs_ = static_cast<double>(playbackFramesPresented_) * frameDurationMs
                         - playbackRunElapsedS_ * 1000.0;
            if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
                prefetchNeighbors();
            }
        }

        if (targetFrame == beforeFrame || (direction > 0 && targetFrame >= maxFrame) || (direction < 0 && targetFrame <= minFrame)) {
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
    layout->addWidget(transportBar_, 0);
    // Dev diagnostics HUD sits below the transport bar. It carries the perf
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
    hooks.stepBack = [this]() { if (prevFrameAction_) prevFrameAction_->trigger(); };
    hooks.stepForward = [this]() { if (nextFrameAction_) nextFrameAction_->trigger(); };

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
    hooks.rateText = [this]() {
        const auto st = playback_.state();
        const double speed = std::abs(st.speed);
        if (speed <= 0.0001) return QStringLiteral("PAUSED");
        return QStringLiteral("%1x").arg(speed, 0, 'g', 2);
    };
    hooks.requestRepaint = [this]() { if (viewer_) viewer_->update(); };

    viewer_->setOverlayHooks(hooks);
}

void MainWindow::setupMenus() {
    auto* fileMenu = menuBar()->addMenu("&File");

    auto* openAction = new QAction("&Open...", this);
    openAction->setShortcut(QKeySequence::Open);
    connect(openAction, &QAction::triggered, this, &MainWindow::openFileDialog);
    fileMenu->addAction(openAction);

    auto* fullscreenAction = new QAction("Toggle &Fullscreen", this);
    fullscreenAction->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Return));
    connect(fullscreenAction, &QAction::triggered, this, [this]() {
        setWindowState(windowState() ^ Qt::WindowFullScreen);
        viewState_.fullscreen = isFullScreen();
        if (transportBar_) transportBar_->setFullscreen(isFullScreen());
        refreshHud("Fullscreen");
    });
    fileMenu->addAction(fullscreenAction);

    fileMenu->addSeparator();
    auto* quitAction = new QAction("&Quit", this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(quitAction);
}

void MainWindow::setupTransportControls() {
    // The QActions carry the transport behavior and are shared by the
    // transport bar, menus, and keyboard. The bar only emits intent; nothing
    // about playback logic lives in the UI widget.
    prevFrameAction_ = new QAction("Previous Frame", this);
    connect(prevFrameAction_, &QAction::triggered, this, [this]() {
        playback_.stepBackward();
        playback_.pause();
        playTimer_.stop();
        stopAudio();
        // Explicit stepping: the user has asked to inspect a frame, not to keep
        // playing. Scrubbing after this must not restart playback.
        userPlayIntent_ = false;
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;

        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, -1, true);
        QString error;
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
            playback_.stepForward();
            loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step);
        } else if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
            prefetchNeighbors();
        }
        refreshHud("Prev Frame");
    });

    playPauseAction_ = new QAction("Play", this);
    connect(playPauseAction_, &QAction::triggered, this, [this]() {
        togglePlayPause();
        refreshHud("Play/Pause");
    });

    nextFrameAction_ = new QAction("Next Frame", this);
    connect(nextFrameAction_, &QAction::triggered, this, [this]() {
        playback_.stepForward();
        playback_.pause();
        playTimer_.stop();
        stopAudio();
        userPlayIntent_ = false;
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;

        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 1, true);
        QString error;
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
            playback_.stepBackward();
            loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step);
        } else if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
            prefetchNeighbors();
        }
        refreshHud("Next Frame");
    });

    // The transport bar owns the slider widget; MainWindow keeps driving it,
    // so every scrub/seek path below is unchanged.
    transportBar_->setFrameText(QStringLiteral("--"));
    connect(transportBar_, &trace::ui::TransportBar::prevFrameClicked,
            prevFrameAction_, &QAction::trigger);
    connect(transportBar_, &trace::ui::TransportBar::playPauseClicked,
            playPauseAction_, &QAction::trigger);
    connect(transportBar_, &trace::ui::TransportBar::nextFrameClicked,
            nextFrameAction_, &QAction::trigger);
    connect(transportBar_, &trace::ui::TransportBar::fullscreenClicked, this, [this]() {
        setWindowState(windowState() ^ Qt::WindowFullScreen);
        viewState_.fullscreen = isFullScreen();
        transportBar_->setFullscreen(isFullScreen());
        refreshHud("Fullscreen");
    });

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

void MainWindow::resizeEvent(QResizeEvent* event) {
    QMainWindow::resizeEvent(event);
    syncScrubPreviewSize();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    if (watched == timelineSlider_ && event->type() == QEvent::Wheel) {
        userPlayIntent_ = false;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::syncScrubPreviewSize() {
    if (!viewer_) return;
    // Clears the frame cache inside the decoder, so it needs the decoder back.
    reclaimDecoder();
    // Device pixels, not logical: on a scaled display the widget is drawn at
    // more pixels than its logical size, and converting to the logical size
    // would put the softness back that this exists to remove.
    const double dpr = viewer_->devicePixelRatioF();
    const QSize px(static_cast<int>(std::lround(viewer_->width() * dpr)),
                   static_cast<int>(std::lround(viewer_->height() * dpr)));
    videoDecoder_.setScrubPreviewSize(px);
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
    playPauseAction_->setEnabled(hasPlayableRange);
    playPauseAction_->setText(playing ? "Pause" : "Play");

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

void MainWindow::captureDecoderTelemetry() {
    if (decoderLeased_) return;
    hudPerf_ = videoDecoder_.perfStats();
    for (int i = 0; i < static_cast<int>(trace::core::IoPhase::Count); ++i) {
        hudIo_[i] = videoDecoder_.ioStats(static_cast<trace::core::IoPhase>(i));
    }
}

void MainWindow::openPath(const QString& path) {
    // Opening another file while storage is slow: supersede the outstanding
    // read so nothing from the previous media is presented afterwards. The
    // guard below then refuses to re-enter until that decode has unwound.
    if (storageBusy_) {
        supersedeInFlightRequests();
        return;
    }
    // Before anything else touches the decoder. Bumps the generation, so a
    // frame the worker is producing for the OUTGOING media can never be
    // inserted against the incoming media, and waits for it to park so
    // close() below is not racing a decode.
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
    frameCache_.clear();
    frameCache_.setWindowCenter(playback_.state().currentFrame);

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
        return;
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

    statusBar()->showMessage("Opened", 1200);
    refreshHud("Open file");
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
bool MainWindow::audioShouldDrive() const {
    if (!audio_.hasAudio()) return false;
    if (!currentMedia_.has_value() || currentMedia_->kind != MediaKind::VideoFile) return false;
    const auto st = playback_.state();
    return st.mode == PlaybackMode::PlayingForward && std::abs(st.speed) <= 1.0001;
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

void MainWindow::postScrubStep(long long frame, int direction) {
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
        if (!result.ok) {
            if (!result.error.isEmpty()) statusBar()->showMessage(result.error, 3000);
            hardError = true;
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
            const long long stride =
                computeScrubStride(std::llabs(pendingScrubFrame_ - activeScrubFrame_));
            postScrubStep(activeScrubFrame_ + dir * stride, dir);
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
        // The ease and the walk budget are gone from this path rather than
        // ported. Both existed only to decide when the synchronous loop should
        // yield: `desired` capped how many consecutive frames one slice
        // covered and `kScrubWalkBudgetMs` capped its time, but neither
        // changed WHICH frames were shown, because the loop always stepped by
        // one. Yielding is now free and happens after every frame, so there is
        // nothing left for them to schedule. What they were really buying --
        // "accelerate when far behind, settle gently on arrival" -- falls out
        // of the pipeline by itself: the chain runs as fast as frames can be
        // decoded and stops when it reaches the pointer.
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
        const long long stride = computeScrubStride(std::llabs(targetFrame - walkFrom));
        postScrubStep(walkFrom + dir * stride, dir);
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
    const QString tc = trace::core::TimeFormat::frameToTimecode(st.currentFrame, fps);

    if (viewState_.readoutMode == PrimaryReadoutMode::Frame) primaryReadout = QString("Frame: %1").arg(st.currentFrame);
    else if (viewState_.readoutMode == PrimaryReadoutMode::Seconds) primaryReadout = QString("Seconds: %1").arg(trace::core::TimeFormat::formatSeconds(sec));
    else primaryReadout = QString("Timecode: %1").arg(tc);

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

            const QString l1 = QString("%1 | %2x%3 | fps %4 | codec %5 | src %6 %7b -> dst %8 | F:%9 | open %10ms | first %11ms")
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
                .arg(QString::number(perf.firstFrameMs, 'f', 2));

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
            const QString resampleState =
                !drawPerf.lastDrawWasScaled
                    ? QStringLiteral("1:1")
                    : (drawPerf.lastDrawWasFiltered
                           ? QStringLiteral("filtered x%1").arg(drawPerf.reduceTaps)
                           : QStringLiteral("NEAREST"));
            const QString l0 = QString("color %1%2 %3 range | display %4x%5 %6 | win %7x%8 | renderer %9")
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
                .arg(viewer_->rendererName());

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

            const QString l3 = QString("cvt/req %1 | ctx-rebuilds %2 | shared %3 | sws %4 | %5 | rev-hit %6%% (%7/%8) | late %9 | walk %10f cache %11cv/%12ms | seek %13/%14 n=%15 | drain %16pk/%17f stale-blocked %18 recov %19")
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
                .arg(perf.recoveredDecodeFailures);

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

            const QString l4 = rateValid
                ? QString("presented %1 / %2 fps (%3%% real time) | frames %4 | elapsed %5s")
                      .arg(QString::number(presentedFps, 'f', 2))
                      .arg(QString::number(vm.fps, 'f', 2))
                      .arg(QString::number(realTimePct, 'f', 1))
                      .arg(playbackFramesPresented_)
                      .arg(QString::number(elapsedS, 'f', 2))
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
                .arg(QString::number(perf.openStreamInfoMs, 'f', 1));

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
            const QString l7c = QString("ui | gap %1/%2ms (avg/max) | over %3ms: %4 of %5 | release %6ms")
                .arg(QString::number(uiGapAvg, 'f', 2))
                .arg(QString::number(uiServiceGapMaxMs_, 'f', 1))
                .arg(QString::number(kUiServiceGapMs, 'f', 0))
                .arg(uiServiceGapsOver_)
                .arg(uiServiceSamples_)
                .arg(QString::number(scrubReleaseLatencyMs_, 'f', 1));

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
            const QString l7f = QString("sample %1 | stride %2 | skipped %3 over %4 steps | ctrl ptr %5 f/s cap %6 f/s | ra-walk %7f/seek")
                .arg(!scrubSamplingEnabled() ? "OFF"
                     : !videoDecoder_.metadata().intraOnly ? "GATED"
                     : scrubStride_ > 1 ? "ON" : "idle")
                .arg(scrubStride_)
                .arg(scrubFramesSkipped_)
                .arg(scrubSampledSteps_)
                .arg(QString::number(ctrlPointerFps_, 'f', 1))
                .arg(QString::number(scrubDecodeFps_, 'f', 1))
                .arg(QString::number(mediaWalkPerSeek(), 'f', 2));

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
                 + "\n" + l7 + "\n" + l7b + "\n" + l7c + "\n" + l7d + "\n" + l7e + "\n" + l7f + "\n" + l8 + "\n" + l9
                 + (l10.isEmpty() ? QString() : "\n" + l10)
                 + "\n" + lio1 + "\n" + lprobe + "\n" + lresp + "\n" + lio2 + "\n" + lio3 + "\n" + lio4;
        } else if (currentMedia_->kind == MediaKind::ImageSequence && currentMedia_->sequence.has_value()) {
            const auto& seq = *currentMedia_->sequence;
            line = QString("Sequence | %1 | %2x%3 ch:%4 | Frame: %5/%6 | Seconds: %7 | Timecode: %8")
                .arg(QString::fromStdString(seq.pattern))
                .arg(currentImage_.has_value() ? currentImage_->width : 0)
                .arg(currentImage_.has_value() ? currentImage_->height : 0)
                .arg(currentImage_.has_value() ? currentImage_->channels : 0)
                .arg(st.currentFrame + 1)
                .arg(seq.frames.size())
                .arg(trace::core::TimeFormat::formatSeconds(sec))
                .arg(tc);
        } else if (currentImage_.has_value()) {
            const auto& im = *currentImage_;
            line = QString("Still | %1 | %2x%3 ch:%4 | Frame: %5/1 | Seconds: %6 | Timecode: %7")
                .arg(im.fileName)
                .arg(im.width)
                .arg(im.height)
                .arg(im.channels)
                .arg(st.currentFrame + 1)
                .arg(trace::core::TimeFormat::formatSeconds(sec))
                .arg(tc);
        }
    }

    overlay_->setTransport(mode, st.currentFrame, st.speed, action.isEmpty() ? primaryReadout : action + " | " + primaryReadout);
    overlay_->setHudLine(line);
    syncTransportBar();
}

void MainWindow::keyPressEvent(QKeyEvent* event) {
    bool needsReload = false;

    switch (event->key()) {
        case Qt::Key_Space:
            togglePlayPause();
            refreshHud("Space");
            return;
        case Qt::Key_M:
            audio_.setMuted(!audio_.isMuted());
            refreshHud(audio_.isMuted() ? "Mute" : "Unmute");
            return;
        case Qt::Key_Left:
            playback_.stepBackward();
            needsReload = true;
            prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, -1, true);
            // Outside the isActive() guard: stepping is a request to inspect a
            // frame whether or not playback happened to be running, and the
            // intent must not survive it into a later scrub release.
            userPlayIntent_ = false;
            if (playTimer_.isActive()) {
                playTimer_.stop();
                stopAudio();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            break;
        case Qt::Key_Right:
            playback_.stepForward();
            needsReload = true;
            prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 1, false);
            userPlayIntent_ = false;
            if (playTimer_.isActive()) {
                playTimer_.stop();
                stopAudio();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            break;
        case Qt::Key_J:
            if (frameSource_ && frameSource_->canPlay()) {
                playback_.jogReverse();
                stopAudio();  // reverse is silent; don't wait a tick for it
                // Reverse is not the 1x forward run a scrub release restores.
                userPlayIntent_ = false;
                prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, -1, true);
                playbackClock_.start();
                playbackAccumulatorMs_ = 0.0;
                if (!playTimer_.isActive()) playTimer_.start();
            }
            refreshHud("J");
            return;
        case Qt::Key_K:
            playback_.pause();
            userPlayIntent_ = false;
            if (playTimer_.isActive()) {
                playTimer_.stop();
                stopAudio();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            refreshHud("K");
            return;
        case Qt::Key_L:
            if (frameSource_ && frameSource_->canPlay()) {
                playback_.jogForward();
                // L at 1x is ordinary forward play and is worth restoring after
                // a drag; the shuttle speeds above it are not, for the same
                // reason they are silent -- an off-speed run is a different
                // gesture, and resuming it at 1x would be the wrong answer.
                userPlayIntent_ = std::abs(playback_.state().speed) <= 1.0001;
                prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, 1, false);
                playbackClock_.start();
                playbackAccumulatorMs_ = 0.0;
                // L at 1x is normal forward play and gets sound; the shuttle
                // speeds above it do not, and this silences them on the way up.
                startAudioForPlayback();
                if (!playTimer_.isActive()) playTimer_.start();
            }
            refreshHud("L");
            return;
        case Qt::Key_F: viewState_.readoutMode = PrimaryReadoutMode::Frame; refreshHud("Readout: Frame"); return;
        case Qt::Key_S: viewState_.readoutMode = PrimaryReadoutMode::Seconds; refreshHud("Readout: Seconds"); return;
        case Qt::Key_T: viewState_.readoutMode = PrimaryReadoutMode::Timecode; refreshHud("Readout: Timecode"); return;
        case Qt::Key_I: viewState_.showInfo = !viewState_.showInfo; refreshHud("I"); return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            viewState_.showHud = !viewState_.showHud;
            overlay_->setVisible(viewState_.showHud);
            refreshHud("Enter");
            return;
        default:
            QMainWindow::keyPressEvent(event);
            return;
    }

    if (needsReload) {
        QString error;
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Step)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
            if (event->key() == Qt::Key_Left) playback_.stepForward();
            else playback_.stepBackward();
        } else if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
            prefetchNeighbors();
        }
    }
    refreshHud(event->key() == Qt::Key_Left ? "Left" : "Right");
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

