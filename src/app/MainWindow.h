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

private:
    void setupUi();
    void setupMenus();
    void setupTransportControls();
    void syncTransportBar();
    void openFileDialog();
    void openPath(const QString& path);
    bool loadCurrentFrame(QString& error, trace::core::VideoDecoderFFmpeg::RequestMode mode = trace::core::VideoDecoderFFmpeg::RequestMode::Playback);
    QString sequenceFramePath(long long frameIndex) const;
    void prefetchNeighbors();
    void togglePlayPause();
    void refreshHud(const QString& action = {});
    // Tell the decoder how big a scrub preview needs to be, in device pixels.
    void syncScrubPreviewSize();
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

    // High-precision presentation scheduling. The timer runs far faster than
    // the frame rate and presentation is gated on the playback accumulator,
    // so the tick interval no longer quantizes the playback rate.
    // Reference interval for the jitter metric: the timer runs at the frame
    // interval, so jitter is reported against that.
    // Set from the media's frame rate at open. Was a hardcoded 42, which made
    // the jitter readout wrong for anything that was not 24fps.
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
    //   stalls  - gaps longer than two refresh intervals, which is what the eye
    //             actually registers as a hitch
    double scrubPaintGapLastMs_ = 0.0;
    double scrubPaintGapMaxMs_ = 0.0;
    double scrubPaintGapSumMs_ = 0.0;
    long long scrubPaintGapSamples_ = 0;
    long long scrubPaintsWasted_ = 0;
    long long scrubPaintStalls_ = 0;

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
    bool suppressSliderSignal_ = false;
    // Set when a playback run stopped because there was nothing further to
    // show -- either the playhead reached the last frame, or the decoder ran
    // out at the tail. Play then restarts the file instead of pressing against
    // an end that cannot move, which is what made the button look dead.
    // Cleared by any move away from the end, so it only ever describes the
    // position playback actually stopped at.
    bool playbackAtEnd_ = false;
    long long playbackEndFrame_ = -1;
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
