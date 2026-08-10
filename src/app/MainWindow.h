#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <optional>
#include <memory>
#include <deque>
#include "core/MediaItem.h"
#include "core/ViewState.h"
#include "core/PlaybackController.h"
#include "core/StillImageLoader.h"
#include "core/FrameCache.h"
#include "core/VideoDecoderFFmpeg.h"
#include "core/FrameSource.h"
#include "core/VideoFrameSource.h"
#include "core/ScrubDecodeWorker.h"
#include "core/AudioOutput.h"

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QDragEnterEvent;
class QDropEvent;

class QSlider;
class QAction;
QT_END_NAMESPACE

namespace trace::ui {
class ViewerWidget;
class TransportOverlay;
class TransportBar;
}

namespace trace::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();
    ~MainWindow() override;

    // Opens a media path supplied on the command line, exactly as File > Open
    // would. Invalid or missing paths are ignored so a bad argument never
    // blocks startup. Exists so playback can be driven from a script.
    void openMediaPath(const QString& path);

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    // Only for the timeline slider, and only to classify a wheel notch. See
    // userPlayIntent_: a wheel over the groove is a stepping gesture that
    // arrives as a bare valueChanged with no press and no release, so it is the
    // one way into the scrub lambdas that is not part of a drag.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void setupUi();
    void setupMenus();
    void setupTransportControls();
    void syncTransportBar();
    // Wires the renderer-composited overlay spike to the existing actions.
    void installOverlayHooks();
    void openFileDialog();
    void openPath(const QString& path);
    bool loadCurrentFrame(QString& error, trace::core::VideoDecoderFFmpeg::RequestMode mode = trace::core::VideoDecoderFFmpeg::RequestMode::Playback);
    QString sequenceFramePath(long long frameIndex) const;
    void prefetchNeighbors();
    void togglePlayPause();
    // Starts a playback run: request mode, audio, clocks and the whole set of
    // cadence/telemetry counters, then the timer. The caller puts playback_ into
    // the mode it wants first; this only starts the machinery for it.
    //
    // Extracted so Play and resume-after-scrub cannot drift apart. Resume needs
    // every counter reset that Play does -- skipping them makes the HUD
    // misreport the resumed run, and step 6's cadence work would start from
    // poisoned counters.
    void startPlaybackRun();
    // The timeline-and-telemetry half of starting a run, without the decoder
    // request or the audio. Extracted because J and L must NOT take the audio
    // half -- reverse is silent, and so is L above 1x -- but they need every
    // clock in here, and the one that matters is sessionClock_: the GATE E
    // deadline timeline is built on it, and a run that starts without it reads
    // now == 0 forever, so the armed delay grows by a frame period every tick
    // and playback decays quadratically (plan section 29.2).
    void beginPlaybackTimeline();
    // Restores playback after a scrub gesture, iff the user never asked for it
    // to stop. Called once the release has landed its exact frame.
    void resumePlaybackAfterScrub();

    // GATE E step 1. Advances the grid slot and re-arms playTimer_ for that
    // slot's absolute deadline. Called from a scope guard on EVERY exit path of
    // the playback tick, because the timer no longer free-runs: a return that
    // does not re-arm is a hang, not a dropped frame.
    //
    // Does nothing when playTimer_ is not active, which is how every existing
    // playTimer_.stop() keeps working untouched.
    void armNextPresent();
    // Establishes the timeline for a run: epoch at now, slot 0, period from the
    // source's exact rational and the current speed. Idempotent -- called from
    // every tick, does something only when the timeline is unset or the period
    // changed under it (a J-K-L speed change), because a new rate against an old
    // epoch is a schedule with a step discontinuity in it.
    void syncPresentTimeline(double frameDurationMs);
    void refreshHud(const QString& action = {});
    // Tell the decoder how big a scrub preview needs to be, in device pixels.
    void syncScrubPreviewSize();
    // Tells the decoder whether the installed renderer can take Y/U/V planes
    // (GATE C). Asked of the adopted renderer, not of TRACE_RENDERER.
    void syncPlanarOutput();
    bool isVideoScrubActive() const;
    void queueVideoScrubFrame(long long frameIndex);
    void flushVideoScrub(bool forceExact);
    trace::core::VideoFrameSource* videoFrameSource();
    void prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode mode, int direction = 1, bool clearQueue = false);
    // Audio drives playback timing when it is running, so these decide whether
    // this playback run is audio-clocked and stop the device the moment it
    // stops being 1x forward.
    bool audioShouldDrive() const;
    void startAudioForPlayback();
    void stopAudio();

    // Runs while a remote read is outstanding: keeps the event loop alive so
    // the window still repaints and input is still accepted, and raises the
    // buffering state once the wait is long enough for a user to notice.
    void pumpDuringStorageStall(double waitedMs);
    bool storageBusy() const { return storageBusy_; }

    // Invalidates any request already in flight, so its result is discarded
    // instead of presented. Call before changing what the user is looking at.
    // Returns the new generation.
    long long supersedeInFlightRequests();

    // The decoder lease. There is one VideoDecoderFFmpeg and exactly one owner
    // of it at any instant: this thread by default, the scrub worker for the
    // duration of a drag. While the lease is out the UI thread touches nothing
    // on the decoder -- not decodeFrameAt, not perfStats, not ioStats -- which
    // is why the HUD reads a snapshot the worker publishes with each result.
    void grantDecoderLease();
    // Raise cancellation, wait for the worker to park outside the decoder, and
    // take ownership back. Bounded by one cancellation checkpoint, which is
    // less than the UI thread already pays per frame of a synchronous walk.
    // Returns the wait in ms; 0 when no lease was out.
    double reclaimDecoder();
    bool decoderLeased_ = false;

    // One step of the async shuttle. Posts the next walk frame if nothing is
    // already in flight; the chain continues from onScrubResult.
    void postScrubStep(long long frame, int direction);
    void onScrubResult();
    // Which way the request currently in flight was walking. 0 when nothing is
    // outstanding. Compared against the direction the pointer now implies, so
    // a reversal can abandon a walk that is heading away from it -- the one
    // case where a pointer move is allowed to invalidate work in flight.
    int scrubInFlightDir_ = 0;

    // ---- Reverse shuttle -------------------------------------------------
    //
    // Continuous reverse runs decode on the SAME worker and under the SAME
    // lease as the drag, and differ in one thing: the target is arithmetic
    // rather than a pointer. At speed S the frame wanted next is always
    // `lastAsked - S`, so the worker can be kept running ahead without
    // speculating about anything -- which is why the lookahead declined for the
    // drag at plan section 15.3 is the right answer here and the wrong one
    // there. Measured idle at reverse 1x: 80% on 4K H.264, 93% on 1080p.
    //
    // Results land in a queue instead of being presented on arrival, and the
    // playback tick pops one per slot. That is the whole cadence fix: a 130ms
    // GOP walk is absorbed by the queue instead of landing inside a 41.67ms
    // presentation slot.
    // Present accounting for one presented frame, shared by forward playback
    // and the reverse shuttle so both are measured by one instrument.
    void notePresentedPlaybackFrame(double frameDurationMs);

    void startShuttleRun(int direction, int stride);
    // True while `frame` is still inside the media. The head and the tail are
    // different expressions, which is why this is a function and not a `< 0`
    // test repeated at three call sites.
    bool shuttleTargetInRange(long long frame) const;
    // Reclaims the lease, drops the queue, and lands exactly on the frame that
    // was last PRESENTED -- never on the one the arithmetic had reached. Safe
    // to call when no run is active.
    void endShuttleRun(bool landExactly);
    // Keeps the pipeline full: posts the next target while the queue is under
    // its high-water mark and nothing is in flight. Chained from onScrubResult.
    void pumpShuttleQueue();
    // Pops one frame and puts it on screen. False when the queue is empty,
    // which is a starve and is held rather than decoded on this thread.
    bool presentQueuedShuttleFrame();
    bool shuttleRunActive_ = false;
    // +1 or -1. The shuttle is direction-agnostic: accelerated FORWARD had
    // exactly the reverse fault -- speed carried in the tick rate with one frame
    // per present, so achieved speed was capped by per-frame decode cost. ProRes
    // 4444 asked for 2x and delivered 1.00x. One mechanism serves both, which is
    // also what stops the two drifting apart.
    int shuttleDir_ = -1;

    // ---- The keyframe snap (owner-approved 2026-08-10) -------------------
    //
    // At high reverse speeds on a long-GOP codec, a target that falls mid-GOP
    // costs a seek to the keyframe PLUS a walk up to it -- measured ~71ms on
    // 1080p against a ~30ms intercept -- and that walk buys nothing, because at
    // this speed only one frame per GOP is ever shown. Snapping the target ONTO
    // the keyframe grid removes the walk entirely.
    //
    // This is the mechanism section 15's INTRA_ONLY gate exists to refuse, and
    // the reason it is safe here is that the sample points are CHOSEN rather
    // than arbitrary: a strided step that lands mid-GOP pays for a region the
    // decoder has to open from scratch, and a snapped one lands where the
    // decoder can already start.
    //
    // Owner decision: accurate 30x at a stable ~15 presentations/second, rather
    // than a smoother picture at a lower speed. So the speed stays exact and the
    // PRESENTATION RATE is what gives: one keyframe per present, paced so the
    // content advances at exactly speed x fps.
    //
    // Learned EXACTLY, from keyframe POSITIONS rather than from a statistic
    // over them. When a request for frame T seeks and walks W frames to reach
    // it, the keyframe it landed on is at T - W: that is an exact observation of
    // where a keyframe is, not an estimate of how far apart they are. Two of
    // them give the spacing exactly.
    //
    // The first attempt used `max(walk) + 1` and it converged FROM BELOW and
    // stopped short -- it read gop 41 on a file whose GOP is 48, so every
    // "snapped" target missed the grid and still walked. A statistic over a
    // quantity is not the quantity; the positions were available all along.
    //
    // This is a STRUCTURAL measurement -- where the keyframes are -- not a cost
    // one, which is what separates it from the four scrub-gate inferences that
    // all measured wrong.
    long long shuttleGop_ = 0;
    // A frame index known to BE a keyframe. The grid is anchored on it rather
    // than on multiples of the spacing, so a file whose first keyframe is not at
    // 0, or whose spacing changes partway, still snaps onto real keyframes.
    long long shuttleKfAnchor_ = -1;
    // Frames advanced per presented frame. Equals the stride normally, and the
    // GOP when snapping. The presentation period is scaled by advance/stride, so
    // the achieved speed stays exactly the commanded one either way.
    long long shuttleAdvance_ = 1;
    bool shuttleSnapping_ = false;
    // Snap once a single presented frame per GOP is all the speed leaves room
    // for. Below that the walk is amortised over several presented frames and is
    // worth paying.
    bool shuttleShouldSnap() const;
    long long shuttleSnapTarget(long long ideal) const;
    // Frames advanced per presented frame. 1 today; the sampling ladder is what
    // makes it >1, and it is the COMMANDED speed rather than an estimate --
    // nothing the decoder does may feed back into it, which is what keeps it
    // clear of the runaway that killed three of section 15's four failed gates.
    int shuttleStride_ = 1;
    long long shuttleNextTarget_ = -1;
    long long shuttleLastPresented_ = -1;
    // The frame AND what was asked for to get it. Carrying both is what keeps
    // the HUD's `target`/`shown`/`delta` honest during a run: without the
    // request, `target` went on reading whatever the last scrub asked for and
    // `delta` reported a difference between two unrelated numbers. Telemetry
    // that asserts its own correctness is how the July 2026 scrub fault survived.
    struct ShuttleFrame {
        long long requested = -1;
        trace::core::VideoFrame frame;
    };
    std::deque<ShuttleFrame> shuttleQueue_;
    long long shuttleStarves_ = 0;
    long long shuttleQueueMaxSeen_ = 0;

    // Decoder telemetry as of the last time it was safe to read. Refreshed
    // from the live decoder when this thread owns it, and from the worker's
    // published snapshot when it does not, so refreshHud has one source and
    // never races the worker.
    void captureDecoderTelemetry();
    trace::core::VideoPerfStats hudPerf_;
    trace::core::IoPhaseStats hudIo_[static_cast<int>(trace::core::IoPhase::Count)];

    trace::ui::ViewerWidget* viewer_ = nullptr;
    trace::ui::TransportOverlay* overlay_ = nullptr;
    trace::ui::TransportBar* transportBar_ = nullptr;
    QAction* prevFrameAction_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* nextFrameAction_ = nullptr;
    QSlider* timelineSlider_ = nullptr;

    trace::core::PlaybackController playback_;
    trace::core::ViewState viewState_;
    trace::core::StillImageLoader stillLoader_;
    trace::core::FrameCache frameCache_{1};
    trace::core::VideoDecoderFFmpeg videoDecoder_;
    // Declared after the decoder so it is destroyed BEFORE it: the worker holds
    // a pointer to the decoder, and a member destroyed in the other order could
    // outlive what it points at. The destructor stops it explicitly as well --
    // this ordering is the backstop, not the mechanism.
    trace::core::ScrubDecodeWorker scrubWorker_;
    trace::core::AudioOutput audio_;
    std::unique_ptr<trace::core::FrameSource> frameSource_;
    QTimer playTimer_;
    QTimer scrubTimer_;
    QElapsedTimer playbackClock_;
    double playbackAccumulatorMs_ = 0.0;
    // Presented-rate accounting: measured from wall clock over the current
    // playback run, so it reflects what the viewer actually saw rather than
    // what was requested. Reset each time playback starts.
    QElapsedTimer playbackRateClock_;
    long long playbackFramesPresented_ = 0;
    // Sampled on each presented frame so the readout freezes when playback
    // stops instead of decaying as the idle clock keeps running.
    double playbackRunElapsedS_ = 0.0;

    // GATE E step 1 -- the presentation timeline.
    //
    // The tick used to be a FIXED integer-millisecond periodic timer at
    // floor(1000/fps): 41ms against a 41.667ms frame. Presents therefore landed
    // on a 41ms grid and every interval between two of them was 41ms or 82ms,
    // never 41.667 -- 61 frames running 1.6% fast, then one held double, every
    // 2.6s, on every file. That is the beat plan section 23 measured (median
    // spacing 61-62 on all six runs) and it is what this replaces.
    //
    // The timer is now re-armed every wake against an ABSOLUTE deadline:
    //
    //     deadline(slot) = presentEpochNs_ + slot * presentPeriodNs_
    //
    // computed from the source's exact rational (FrameSource::fpsRational), in
    // nanoseconds, never rounded. Only the delay handed to QTimer is rounded to
    // a millisecond, and because the next delay is computed from the next
    // absolute deadline rather than from this one, that rounding cannot
    // accumulate -- the arms alternate 41/42 and the mean is the true period.
    //
    // presentSlot_ is a GRID SLOT, not a frame count. It advances on every wake
    // whether or not a frame was presented, so the heartbeat stays regular and
    // "which frame" remains entirely the audio clock's question (plan section
    // 24.3: audio owns rate and position, the schedule owns phase).
    qint64 presentEpochNs_ = -1;
    long long presentSlot_ = 0;
    double presentPeriodNs_ = 0.0;
    // The deadline the current wake was armed for, and how far past it the wake
    // actually landed. The latter is the honest presentation-latency figure now:
    // the old one measured the wall-clock accumulator's surplus, which under a
    // deadline schedule says nothing.
    qint64 presentTargetNs_ = -1;
    double presentSlotLatencyMs_ = 0.0;
    // Times the schedule was re-phased because a handler overran its slot. This
    // is the analogue of the old 4-frame accumulator backlog cap: a run that
    // stalled resumes AT RATE rather than fast-forwarding through the arrears.
    // Non-zero here means cost overrun (cause B), which GATE E does not fix and
    // must not be blamed for -- it is the honest place that shows up now.
    long long presentRephaseCount_ = 0;
    // Reference interval for the jitter metric: the deadline the wake was armed
    // for, so jitter measures the scheduler against its own intent rather than
    // against a nominal rate. Still an int for the HUD's benefit.
    int schedulerIntervalMs_ = 42;
    QElapsedTimer schedulerTickClock_;
    long long schedulerTicks_ = 0;
    long long presentSamples_ = 0;
    double lastTickJitterMs_ = 0.0;
    double avgTickJitterMs_ = 0.0;
    double maxTickJitterMs_ = 0.0;
    double lastPresentLatencyMs_ = 0.0;
    double avgPresentLatencyMs_ = 0.0;
    double maxPresentLatencyMs_ = 0.0;
    double lastDriftMs_ = 0.0;

    // A/V sync accounting for the current run: how far the frame just presented
    // sits from where the audio clock says it should be, in milliseconds.
    // Positive = picture ahead of sound.
    bool audioDriving_ = false;
    // Watchdog against a stalled audio clock holding the playhead still.
    static constexpr qint64 kAudioStallMs = 300;
    QElapsedTimer audioClockStall_;
    double lastAudioClockS_ = -1.0;
    bool audioClockStalled_ = false;
    double lastAvSyncMs_ = 0.0;
    double maxAvSyncMs_ = 0.0;
    long long audioRepeatedFrames_ = 0;  // clock did not advance a whole frame
    long long audioSkippedFrames_ = 0;   // clock ran past the next frame

    // The audio clock is a control loop, so reading it used to step it: the HUD
    // and the tick both called the old mutating clockSeconds(), doubling the
    // gain and making playback timing depend on whether the HUD was visible.
    // Measured between tick entries, so it catches a stray step from anywhere.
    // True while audio is running but the device has not begun emitting sound.
    // Video stays on the wall clock for that window.
    bool audioClockPriming_ = false;
    long long lastClockUpdateMark_ = -1;
    long long lastClockUpdatesPerTick_ = 0;
    long long maxClockUpdatesPerTick_ = 0;

    // Frame-cycle accounting. The tick handler runs decode+convert+handoff and
    // returns; the viewer's paintEvent runs later, in the event loop, so paint
    // is NOT additive to handler time. Period = handler + everything outside.
    QElapsedTimer frameCycleClock_;
    double lastHandlerMs_ = 0.0;
    double avgHandlerMs_ = 0.0;
    double lastPeriodMs_ = 0.0;
    double avgPeriodMs_ = 0.0;
    double lastOutsideMs_ = 0.0;
    double avgOutsideMs_ = 0.0;
    double maxPeriodMs_ = 0.0;
    long long cycleSamples_ = 0;

    // Cadence distribution for the current run. The presented RATE averages, and
    // reads 98-99% under two unrelated faults, so it cannot say which one a file
    // is suffering from -- see the long comment at the present site. These are
    // the numbers that can.
    //
    // Every interval between consecutive PRESENTS (not ticks: a held frame
    // produces no present, so a doubled interval only exists here), capped so a
    // long session cannot grow this without bound. 10s at 24fps is 240 samples.
    static constexpr std::size_t kCadenceSampleCap = 4096;
    std::vector<double> cadenceGapsMs_;
    // Presented-frame index of each interval longer than 1.5x the budget. The
    // SPACING between these is what separates a regular beat from ragged
    // overrun, which the count alone cannot.
    std::vector<long long> cadenceLongAt_;
    // Frame budget this tick worked to, published for the handler scope guard,
    // which runs before the local is in scope.
    double tickFrameDurationMs_ = 0.0;
    long long handlerSamples_ = 0;
    long long handlerOverBudget_ = 0;
    double maxHandlerMs_ = 0.0;
    // Wall-clock span between the first and last presented frame, which is
    // what (N-1) frame intervals actually cover.
    qint64 firstPresentNs_ = -1;
    qint64 lastPresentNs_ = -1;
    QElapsedTimer sessionClock_;
    // Measured cost of one frame of the scrub shuttle walk, which decides how
    // far a single slice may walk before jumping instead. Measured rather than
    // derived from VideoPerfStats averages: those are pooled across seek-walk
    // decodes and read ~4x the true sequential cost (dec 0.07 last vs 5.02
    // avg on the 1080p clip), which would collapse the walk to a jump.
    double scrubWalkPerFrameMs_ = 1.0;
    // Presentation pacing for the shuttle. Without it a slice that lands a run
    // of cache hits paints a dozen frames inside 8ms -- far faster than the
    // display can show them, so most are overwritten before a refresh ever
    // samples them -- and then stalls on the next miss. The eye sees a couple
    // of frames, a freeze, a couple more: the "jumpy"/"steppy" report. Frames
    // are paced to one per refresh interval so each one is actually seen.
    QElapsedTimer scrubPresentClock_;
    qint64 scrubLastPresentNs_ = -1;
    // Smoothness, as opposed to throughput. Frames-per-second and lag can both
    // look excellent while the motion feels bad, because they say nothing about
    // *when* frames land: a slice that paints twelve frames in 8ms and then
    // stalls 30ms on a miss scores perfectly and reads as a stutter. These
    // measure the interval between consecutive paints during a drag.
    //   wasted  - painted sooner than the display could possibly show the
    //             previous one, so the previous frame was overwritten unseen
    //   stalls  - gaps longer than two refresh intervals. RELATIVE TO THE
    //             DISPLAY, so it is 8.3ms on a 240Hz panel and 33.3ms on a
    //             60Hz one: it says "slower than the panel could have shown
    //             it", which is the right companion to `wasted` and is NOT a
    //             measure of what the eye registers as a hitch, whatever this
    //             comment used to claim.
    //   hitch   - gaps over kScrubHitchMs, an absolute duration. This is the
    //             one to compare across sessions; `stalls` silently changes
    //             unit when the display mode does.
    double scrubPaintGapLastMs_ = 0.0;
    double scrubPaintGapMaxMs_ = 0.0;
    double scrubPaintGapSumMs_ = 0.0;
    long long scrubPaintGapSamples_ = 0;
    long long scrubPaintsWasted_ = 0;
    long long scrubPaintStalls_ = 0;
    long long scrubPaintHitches_ = 0;

    // How long the event loop went unserviced, which is a different question
    // from how long a paint took to arrive. Every other scrub metric is
    // measured from inside the work; this one is measured from outside it, by a
    // 1ms timer that can only fire when the UI thread is back in the event
    // loop. A gap here is, literally, a stretch during which no mouse move was
    // delivered, no repaint happened and the slider handle could not move --
    // the owner's "slider not keeping up with the pull".
    //
    // It runs only during a drag: a 1ms timer left running would add wakeups to
    // the validated playback path for no reason.
    QTimer uiServiceTimer_;
    QElapsedTimer uiServiceClock_;
    qint64 uiServiceLastNs_ = -1;
    double uiServiceGapMaxMs_ = 0.0;
    double uiServiceGapSumMs_ = 0.0;
    long long uiServiceSamples_ = 0;
    // Gaps long enough to be seen as the handle detaching from the pointer.
    // One display refresh at 60Hz; deliberately not tied to the actual panel
    // rate, so the figure is comparable across machines.
    static constexpr double kUiServiceGapMs = 16.0;
    long long uiServiceGapsOver_ = 0;
    void startUiServiceMeasurement();
    void stopUiServiceMeasurement();
    // Release -> exact frame on screen. The one place a blocking decode is the
    // right answer, so it is reported separately from the drag rather than
    // pooled into its worst gap.
    double scrubReleaseLatencyMs_ = 0.0;

    // The lag model. Step 5 made the UI responsive while the picture can still
    // trail the pointer badly on heavy media, and "how far behind" is not one
    // question but four: is the decoder simply too slow, is it working on the
    // wrong frames, is the cache failing, or is the pointer just moving faster
    // than anything could follow. These separate them.
    //
    // Every one of them resets per gesture: the interesting figure is this
    // drag, not the worst since launch.
    QElapsedTimer scrubGestureClock_;
    // Where the pointer has been, and when. Kept so a presented frame can be
    // asked the only question the user actually cares about -- how long ago
    // was my hand here -- which no throughput number answers.
    struct PointerSample { long long frame; qint64 ns; };
    std::deque<PointerSample> pointerTrail_;
    static constexpr size_t kPointerTrailMax = 1024;
    // Direction inferred from recent pointer targets -- never from decoder
    // position, which lags and would report a reversal late. +1/-1/0.
    int scrubDirection_ = 0;
    long long scrubReversals_ = 0;
    // Pointer travel, in source frames per second. What the hand is asking for.
    double scrubPointerFps_ = 0.0;
    long long scrubPointerFramesTotal_ = 0;
    long long scrubLastPointerFrame_ = -1;
    // Frames actually put on screen per second during the gesture. What the
    // decoder can supply. The ratio of these two IS the lag model.
    long long scrubPresentedFrames_ = 0;
    double scrubDecodeFps_ = 0.0;
    // Frames behind the pointer: worst during the gesture, and where it ended.
    long long scrubLagMaxFrames_ = 0;
    long long scrubLagLastFrames_ = 0;
    // How stale the picture is, in time rather than frames -- the figure that
    // says "the image is a second behind your hand".
    double scrubPointerToPreviewMs_ = 0.0;
    double scrubPointerToPreviewMaxMs_ = 0.0;
    // Longest GOP walk paid for during this gesture, and how many seeks it
    // took. Distinguishes "the codec is slow" from "we keep re-walking".
    long long scrubWalkMaxFrames_ = 0;
    long long scrubSeeksAtGestureStart_ = 0;
    void resetScrubLagModel();
    void notePointerTarget(long long frame);
    void notePresentedScrubFrame(long long frame);

    // Preview sampling. Short-window estimates, kept separate from the
    // gesture-cumulative figures above: those are for reading afterwards, these
    // have to steer while the drag is running and a cumulative average adapts
    // far too slowly to a reversal or a change of speed.
    //
    // Demand is short-window: a drag that starts slow and then whips must raise
    // the stride now, not average it away.
    double ctrlPointerFps_ = 0.0;
    qint64 ctrlLastPointerNs_ = -1;
    // Capacity is deliberately NOT a short window, and not a cost average
    // either. An EMA of per-request cost is dominated by whichever of hits and
    // misses happened to come last -- measured reading 0.17ms after a run of
    // cache hits on a file whose true mixed cost is ~5ms, which collapses the
    // stride exactly when a heavy stretch begins. Frames actually presented per
    // second over the whole gesture is stable, includes hits and misses in
    // their real proportion, and is close to invariant under striding, because
    // one presented frame costs one decode whatever the stride.
    // (scrubDecodeFps_ above is that measure; capacity reads it.)
    // How many source frames the next request advances. 1 is the shipped
    // behaviour -- every frame, in order -- and is what the controller returns
    // whenever the decoder can keep up.
    long long scrubStride_ = 1;
    long long scrubFramesSkipped_ = 0;
    long long scrubSampledSteps_ = 0;
    // What random access costs on this media: the mean number of frames a
    // request has had to walk to reach its target, accumulated over the media
    // rather than decayed. An EMA was tried and is wrong here for the same
    // reason as above -- it reads whatever the last few requests did, so it
    // collapses to near zero during a run of cache hits and reports a long-GOP
    // file as having free random access. A latch was tried too and is wrong in
    // the other direction: ProRes seeks occasionally land short and walk a few
    // frames, and one such walk disabled sampling for the session.
    // Counted per SEEK, not per request. A forward drag performs almost no
    // seeks, so requests that never sought are no evidence at all about what
    // random access costs -- averaging them in diluted a long-GOP file's mean
    // below the threshold on any gesture with a forward segment, which turned
    // sampling on during exactly the backward stretches it must not run in.
    // Measured on a 4K H.264 reversal set: stalls 2 of 437 -> 13 of 199.
    long long mediaWalkFramesTotal_ = 0;
    long long mediaSeekCount_ = 0;
    long long mediaSeeksSeen_ = 0;
    double mediaWalkPerSeek() const {
        return mediaSeekCount_ > 0
            ? static_cast<double>(mediaWalkFramesTotal_) / static_cast<double>(mediaSeekCount_)
            : 0.0;
    }
    // Above this, a strided step is expected to leave the region the decoder
    // has already opened and pay for a new one. One frame is deliberately
    // strict: on an all-intra file the true mean is a small fraction.
    static constexpr double kRandomAccessWalkLimit = 1.0;
    long long computeScrubStride(long long gap) const;
    bool suppressSliderSignal_ = false;
    // Set when a playback run stopped because there was nothing further to
    // show -- either the playhead reached the last frame, or the decoder ran
    // out at the tail. Play then restarts the file instead of pressing against
    // an end that cannot move, which is what made the button look dead.
    // Cleared by any move away from the end, so it only ever describes the
    // position playback actually stopped at.
    bool playbackAtEnd_ = false;
    long long playbackEndFrame_ = -1;
    // Intent, not mechanism: the user has asked for playback and has not asked
    // for it to stop. Distinct from playTimer_.isActive(), which is whether the
    // mechanism is currently running -- a scrub suspends the mechanism without
    // touching the intent, which is what makes playback survive a drag.
    //
    // Set true only by Play (and by L at 1x, which is the same thing). Set
    // false wherever the user asks for something other than 1x forward
    // playback: pause, stepping, J-K-L off-speed or reverse, opening media,
    // and running out of frames. The scrub path never writes it.
    //
    // Deliberately NOT a snapshot captured when the drag begins. A groove click
    // sets the slider value before QSlider emits sliderPressed, so the pause in
    // the valueChanged lambda has already run by the time a capture there could
    // read the state -- it would record "was paused" for a click that began
    // during playback, and gating the capture on isSliderDown() fails the same
    // way. Carrying the intent instead makes the emission order irrelevant, and
    // makes a Play or Pause pressed while the release is still resolving win:
    // it flips the intent, and the restore reads the intent.
    bool userPlayIntent_ = false;
    bool scrubbing_ = false;
    // A press is a jump, not the first step of a shuttle. The slider does an
    // absolute set on a groove click, so the value arrives before the pointer
    // has moved anywhere -- and shuttling to it walked every frame in between,
    // which on heavy media is seconds of decoding to reach a frame the user
    // pointed straight at. The first flush after a press lands exactly; only
    // movement after that shuttles.
    bool scrubJumpPending_ = false;
    // Whether the frame currently on screen was landed exactly, at full
    // resolution, through Step. Lets the release skip re-decoding a frame the
    // press already put there accurately -- never a soft or approximate one.
    bool scrubShownExact_ = false;
    long long pendingScrubFrame_ = -1;
    long long activeScrubFrame_ = -1;

    double lastFrameHandoffMs_ = 0.0;
    double avgFrameHandoffMs_ = 0.0;
    long long frameHandoffSamples_ = 0;

    // Storage responsiveness state.
    //
    // storageBusy_ is a re-entrancy guard, and it is the load-bearing part:
    // pumping events inside a decoder call means a timer tick or a key press
    // can re-enter the decoder while FFmpeg is mid-read. Every path that
    // drives the decoder checks it and defers instead.
    bool storageBusy_ = false;
    bool buffering_ = false;
    double storageWaitMs_ = 0.0;
    long long bufferingEvents_ = 0;
    double bufferingMsTotal_ = 0.0;
    double maxStorageWaitMs_ = 0.0;
    QElapsedTimer bufferingClock_;
    // A user action arrived while storage was busy; applied once it clears.
    // Monotonic. Bumped by anything that invalidates work already in flight:
    // a new scrub target, opening or closing media. A result produced under an
    // older generation is dropped at the boundary in loadCurrentFrame rather
    // than presented after the user has moved on.
    //
    // Today the only way the generation can change mid-request is a remote read
    // pumping the event loop, which is what this began as (`ioCancelCount_`).
    // It is generalised and named for what it means because the async scrub
    // worker needs exactly this and needs it enforced in one place: latest
    // target wins, and a superseded result never reaches the viewer.
    long long requestGeneration_ = 0;
    // What the last completed request asked for, and what it actually
    // delivered. Read off the returned frame rather than from decoder
    // internals, so a cache hit reports as honestly as a fresh decode.
    long long lastRequestedFrame_ = -1;
    long long lastDeliveredFrame_ = -1;
    // Results actually thrown away because the world moved while they were in
    // flight. Distinct from the generation, which counts how often the target
    // changed -- most of those supersede nothing because nothing was running.
    // This is the number that should stay at 0 on local media and go non-zero
    // on a slow remote source, and it is what to watch when decode moves off
    // the UI thread.
    long long supersededResults_ = 0;
    // Long enough that ordinary reads never flicker the indicator, short
    // enough that a real stall is acknowledged before it feels like a hang.
    static constexpr double kBufferingVisibleMs = 150.0;

    std::optional<trace::core::MediaItem> currentMedia_;
    std::optional<trace::core::LoadedImageInfo> currentImage_;
    // The video frame most recently pulled from the decoder. Held across
    // requests so the decoder's conversion pool sees a steady number of
    // outstanding references rather than one that collapses between frames.
    trace::core::VideoFrame videoFrameBuffer_;
};

} // namespace trace::app
