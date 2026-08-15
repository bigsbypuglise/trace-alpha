#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QByteArray>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <optional>
#include <memory>
#include <deque>
#include <vector>
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
#include "app/ShortcutTable.h"
#include "app/MediaShare.h"
#include "app/RecentFiles.h"
#include "app/MovieInspector.h"
#include "render/ViewTransform.h"

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QDragEnterEvent;
class QDropEvent;

class QSlider;
class QAction;
class QActionGroup;
class QMenu;
QT_END_NAMESPACE

// Forward-declared at GLOBAL scope, and that placement is the point: written
// inside namespace trace::app it would declare a brand new trace::app::tagRECT
// that no Win32 call can be given. windows.h is deliberately not included here
// -- this header is reached by most of the application, and pulling the Win32
// world into all of it to name one pointer type would be a poor trade.
struct tagRECT;

namespace trace::ui {
class ViewerWidget;
class TransportOverlay;
class TransportBar;
}

namespace trace::app {

class OverlayAccessibility;
class ShortcutsWindow;

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
    // Reapplies the aspect lock on the way back to normal from fullscreen,
    // maximize or snap -- but only when the restored geometry is actually the
    // wrong shape, so a correctly-shaped restore keeps its position.
    void changeEvent(QEvent* event) override;
    // Only for the timeline slider, and only to classify a wheel notch. See
    // userPlayIntent_: a wheel over the groove is a stepping gesture that
    // arrives as a bare valueChanged with no press and no release, so it is the
    // one way into the scrub lambdas that is not part of a drag.
    bool eventFilter(QObject* watched, QEvent* event) override;
    // MEASUREMENT ONLY at this stage: it counts WM_SIZING / WM_ENTERSIZEMOVE /
    // WM_EXITSIZEMOVE and always returns false, so every message continues to
    // its default handling and the window behaves exactly as it did before.
    //
    // It is here rather than in the eventual aspect-lock commit because spec
    // section 4's whole design rests on these three messages arriving on the
    // top-level Qt window, and that has never executed in this project -- there
    // is no other nativeEvent override in src/. Proving they arrive, and how
    // many of them a real drag produces, is a prerequisite for the design
    // rather than part of it.
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;

private:
    void setupUi();
    // The QActions that more than one surface reaches. Runs before setupMenus()
    // because a menu is one consumer of an action, never its definition.
    void setupSharedActions();
    void setupMenus();
    // Walks the built menu bar and warns on stderr when two items in one menu
    // claim the same Alt mnemonic -- which makes the key cycle the highlight
    // rather than activate either. Phase 14 introduced two of them in one menu;
    // see the definition.
    void warnOnDuplicateMnemonics() const;
    void setupTransportControls();
    // Fills shortcuts_. Runs LAST, after every action it lists exists.
    void setupShortcuts();
    // The one place the window's fullscreen state is changed. Re-reads
    // isFullScreen() afterwards rather than assuming the toggle took, so the
    // action's checked state, viewState_ and the button icon all come from what
    // the window manager actually did.
    void toggleFullscreen();
    void setHudVisible(bool visible);
    // The one place the time readout changes. Refuses SMPTE when the source
    // carries no timecode and says why, rather than accepting a mode that would
    // then have to print something else -- a key that changes nothing on screen
    // is the `showInfo` failure phase 2 deleted.
    void setReadoutMode(trace::core::PrimaryReadoutMode mode);
    // Ticks the checked readout and enables/disables everything SMPTE from the
    // one `hasSourceTimecode_` gate, so the menu cannot disagree with the HUD.
    void syncTimeDisplayActions();
    // Re-reads the source's start timecode and parses it ONCE per media open.
    // Also resets the readout out of SMPTE if the new media has none.
    void refreshSourceTimecode();
    // The source timecode at a frame index: start + index, in the source's own
    // drop-frame convention. Only meaningful when hasSourceTimecode_.
    QString sourceTimecodeAt(long long frame) const;
    // Frame index for a source timecode, or -1 if it does not parse, is not
    // this source's convention, or falls outside the media.
    long long frameForSourceTimecode(const QString& text) const;
    void promptGoToFrame();
    void promptGoToTimecode();
    // Spec phase 13. Shows or hides the Movie Inspector, creating it on first
    // use so a session that never opens it pays nothing for it.
    void toggleMovieInspector(bool show);
    // Everything the inspector displays, gathered from state this window already
    // holds. NOTHING here stats a path, opens a file or asks the decoder a
    // question: the file size comes from the open, the colour tags from the
    // metadata read at open, and the viewport from the last paint.
    trace::app::InspectorSnapshot buildInspectorSnapshot() const;
    // Rebuild the inspector's contents, if it exists and is visible.
    //
    // THE ONLY PLACE THAT PUSHES A SNAPSHOT, and it is reached from exactly two
    // routes: the window becoming visible, and inspectorRefreshTimer_. See
    // scheduleInspectorRefresh() for why there is a timer at all.
    void refreshInspector();
    // Arm the coalescing refresh. Cheap enough to call from a resize: it starts
    // a single-shot timer and does nothing else, and it returns immediately when
    // the inspector is not on screen.
    void scheduleInspectorRefresh();

    // --- spec phase 14 -------------------------------------------------------
    // Show or raise the Keyboard Shortcuts window, built from
    // ShortcutTable::rows(). Modeless and created on first use, like the
    // inspector.
    void showKeyboardShortcuts();
    void showAboutDialog();
    // Real content, not a second surface onto About. See its definition.
    void showTraceHelp();
    // Human-readable build identity: version, Qt, renderer in force. One
    // function so About and the Report an Issue mail body cannot disagree about
    // which build the user is running.
    QString buildIdentity() const;

    // Put the window in front of every non-topmost window, or stop doing so.
    // Goes through SetWindowPos rather than setWindowFlag -- see
    // alwaysOnTopAction_ for why that is load-bearing rather than stylistic.
    void applyAlwaysOnTop(bool on);

    // Tear the current media down and return to the empty state, WITHOUT
    // opening anything. Shares openPath's teardown so the two cannot drift into
    // clearing different things.
    void closeMedia();
    // The teardown itself: everything that must be forgotten before either a
    // different file is opened or nothing is. Called by openPath and closeMedia.
    void releaseCurrentMedia();
    // Enable or disable everything that needs media to be open. One function, so
    // a new media-dependent command has one place to be listed rather than a
    // condition repeated at each menu.
    void syncMediaDependentActions();

    // The ONE way the playback rate is commanded, and the only caller that may
    // choose a rate other than through the shuttle ladder. `speed` is signed;
    // 0.5 is the half-speed path, 1.0 is ordinary playback, and the rest of the
    // ladder routes through startShuttleRun exactly as J/L and the buttons do.
    // The ONE place "playback reached the end of the media" decides what to do.
    // Three sites in the tick reach it; all three ask this. Returns true when
    // the playhead has been wrapped and playback should continue.
    bool loopWrap(int direction);

    void setPlaybackSpeed(double speed);
    // Tick the Playback Speed menu from playback_.state().speed. Read back from
    // the controller, never remembered, so the menu cannot claim a rate the
    // engine is not running.
    void syncPlaybackSpeedActions();

    // Copy the frame currently on screen to the clipboard as an image.
    //
    // NOT a clipboard one-liner on the shipping path: since GATE C a full-res
    // frame on d3d11 is three YUV planes and VideoFrame::toQImage() returns null
    // for them by construction. This asks the decoder for an RGB copy at the
    // source's own colorimetry, and refuses a preview-resolution frame rather
    // than quietly copying a soft one.
    void copyCurrentFrame();
    // Spec phase 8. Re-evaluates the Share gate ONCE per media open -- the spec
    // forbids filesystem probing in paint or timeline updates and the gate
    // queries the volume, so this follows refreshSourceTimecode()'s shape
    // exactly: compute at open, cache, and let every surface read the cache.
    void refreshShareState();
    // Pushes the cached gate onto the three actions: enabled state, and the
    // tooltip that says why when it is not. One function so the menu bar, the
    // docked bar's menu and the overlay's menu cannot show three answers.
    void syncShareActions();
    // Pops the Share menu at a GLOBAL point. Both surfaces call this; neither
    // owns the menu.
    void showShareMenu(const QPoint& globalPos);
    void copyMediaFilePath();
    void showMediaInFileExplorer();
    // Spec phase 10. The ONE place the view transform changes, so every action
    // goes through the same apply-and-report path and none of them writes the
    // viewer's state directly.
    void applyViewTransform(const trace::render::ViewTransform& next, const char* action);
    // Ticks Flip Horizontal / Flip Vertical and enables Reset from the transform
    // actually in force, rather than from what the last press was assumed to do.
    void syncViewTransformActions();
    // Spec phase 15. Ticks Actual Size / Fit to Window and enables the four
    // scaling commands from the scale actually in force, for the same reason
    // the transform has its own: the state lives in the viewer and the menu
    // reports it rather than remembering what it last asked for.
    void syncViewScaleActions();
    // Spec phase 9. Runs the installed LucidLink integration's own copy-link
    // command on a worker and reports what it produced. Trace never composes a
    // link; see LucidLinkIntegration.h for why the daemon's REST API is not the
    // mechanism even though it is authoritative.
    void copyLucidLinkForMedia();
    // Asks the integration, off the UI thread, whether it offers a link for the
    // current file. Started only when the volume is a virtual mount, so a local
    // file never loads a third-party shell DLL into this process.
    void startLucidProbe(const QString& path);
    // One validated, exact jump, shared by both prompts. Uses the existing Step
    // path, so neither prompt needed any decoder work.
    void goToFrame(long long frame, const char* action);
    // The spec's temporary shuttle-rate indicator, driven from startShuttle --
    // the one place a shuttle rate is ever chosen -- and delivered to both the
    // docked bar's label and the floating overlay's text from one string.
    void flashRate(const QString& text);
    void syncTransportBar();
    // Wires the renderer-composited overlay spike to the existing actions.
    void installOverlayHooks();
    void openFileDialog();
    // Returns whether media is now open and showing a frame.
    //
    // It returned void until spec phase 11. Open Recent is the first caller that
    // has to DO something different when an open fails -- the spec asks for a
    // missing recent file to be reported with an offer to remove the entry --
    // and inferring failure from a status-bar string would be a second answer to
    // a question this function already knows.
    bool openPath(const QString& path);

    // Spec phase 11. Rebuilds the File > Open Recent submenu from the stored
    // strings. Called when the list CHANGES, not when the menu is shown: the
    // spec forbids probing recent paths at startup and blocking on disconnected
    // ones, and a rebuild that runs on aboutToShow is the natural place for a
    // later change to add a stat() and stall the menu on a dead mount.
    void rebuildRecentMenu();
    // Opens a path chosen from the recent list. Deliberately does NOT probe
    // first -- see the comment on the definition.
    void openRecentPath(const QString& path);
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
    // Frames of media time this present should advance. 1 unless the run is
    // already behind, in which case media time is held real-time and the
    // intervening picture is dropped. 1x forward playback only.
    long long realtimeDropSteps(int direction,
                                const trace::core::PlaybackState& playbackState,
                                double frameDurationMs);
    void refreshHud(const QString& action = {});
    // Tell the decoder how big a scrub preview needs to be, in device pixels.
    void syncScrubPreviewSize();

    // SPEC SECTION 4. The on-screen display aspect of whatever is open, media
    // shape composed with the user's view transform, or 0 when there is nothing
    // to shape the window to.
    double currentDisplayAspect() const;
    // Size the window so the VIDEO CLIENT AREA -- not the outer frame -- is the
    // media's ratio, at natural displayed size where it fits and scaled down
    // where it does not, centred on the monitor the window is already on.
    // Declines in every state where Windows owns the geometry.
    void applyMediaWindowShape();
    // One pass of it. Returns true when the layout did not honour the size it
    // was given, which means the chrome it measured was wrong and another pass
    // is worth running. See applyMediaWindowShape for why that can happen.
    bool applyMediaWindowShapePass(double aspect, int pass);
    // Section 20.4, armed by WM_DPICHANGED through dpiReshapeTimer_. Re-runs the
    // shaping pass IFF the scale factor actually changed, so a move between two
    // monitors that share one does nothing.
    void reshapeAfterDpiChange();
    // Chrome is measured rather than computed: the difference between the window
    // and the viewer is menu bar, status bar, frame, the HUD's own height and
    // the docked transport bar if it is present, and every one of those changes
    // with state. Anything that adds them up from constants is a list that goes
    // stale.
    QSize windowChromeLogical() const;
    bool windowGeometryIsOurs() const;
#ifdef Q_OS_WIN
    // Reshapes a proposed WM_SIZING rect in place, moving only the edges the
    // user is not dragging. Returns whether it changed anything.
    bool constrainSizingRect(::tagRECT* rect, int edge);
#endif
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
    // `batch` is how many consecutive frames from `frame` the one request
    // covers. Defaulted so the reverse shuttle and every other caller keep the
    // original one-frame behaviour without naming it.
    void postScrubStep(long long frame, int direction, long long batch = 1);
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

    // The whole of what a shuttle press does, in the one order that works.
    //
    // Extracted at spec phase 3, BEFORE phases 4 and 5 add the Rewind and
    // Fast-forward buttons as a third and fourth caller. Plan section 29.2 is
    // the standing reason: GATE E was validated on the Play action alone, and
    // for weeks every other path that started the playback timer kept compiling
    // silently while decaying quadratically. A sequence with five steps and no
    // name is a sequence a new caller gets four steps of.
    //
    // `entry` is which rung the ladder starts on -- J/L enter at 1x, the Rewind
    // and Fast-forward buttons at 2x. There is no landing parameter: spec phase
    // 4 measured it and no shuttle press lands the previous run. See the
    // definition for the measurement and for the two halves of the old
    // justification that did not survive it.
    void startShuttle(int direction, trace::core::ShuttleEntry entry);

    // The single exact-frame-step command, reached by Left/Right alone from spec
    // phase 5 on. `delta` is -1 or +1.
    void stepOneFrame(int delta, const char* hudLabel);

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

    // ---- Checkpoint 2 stage one: the bounded playback prefetch ------------
    //
    // IT IS A LOOKAHEAD CACHE AND NEVER A SCHEDULE. The tick's target
    // arithmetic is untouched: the audio clock or the deadline scheduler still
    // decides which frame and when, and this only answers "do I already have
    // it". A queue that assumed the next frame is current+1 would be a second
    // mechanism owning half of "which frame, when", which is exactly the fault
    // cd79d49 removed.
    //
    // Ordinary 1x forward playback only. Reverse and every shuttle rung are
    // already queued by startShuttleRun, and below 1x every frame is presented
    // by owner decision so there is nothing to run ahead of.
    //
    // Default OFF (TRACE_PLAYBACK_QUEUE=0), so the synchronous path is the
    // comparison and the rollback. It is not a second implementation kept in
    // sync -- it is the path that runs when the queue cannot answer.
    struct PlaybackFrame {
        long long requested = -1;
        trace::core::VideoFrame frame;
    };
    std::deque<PlaybackFrame> playbackQueue_;
    // Whether the prefetch owns the decoder for this run. Distinct from the
    // queue being non-empty: the queue is empty at the start of every run and
    // whenever it starves.
    bool playbackPrefetchActive_ = false;
    // The next frame to ask the worker for, or -1 once the arithmetic has run
    // off the end of the media. The shuttle's shuttleNextTarget_ in every
    // respect except the stride, which here is always 1.
    long long playbackPrefetchNext_ = -1;
    // The worker reported it could not produce a frame. Long-GOP media reaches
    // the real end before the frame count says it should, and on the prefetch
    // path that arrives as a result rather than as a return value.
    bool playbackPrefetchExhausted_ = false;
    long long pqStarves_ = 0;
    long long pqAheadDrops_ = 0;
    long long pqReseeds_ = 0;
    long long pqPosted_ = 0;
    long long pqMaxDepth_ = 0;
    long long pqBytes_ = 0;
    long long pqPeakBytes_ = 0;
    // Time the TICK spent blocked on the queue. It must read 0: a non-zero
    // value means something is waiting, and the whole design is that a starve
    // holds rather than waits.
    double pqWaitMaxMs_ = 0.0;

    // Depth for this media, from the byte budget and the count cap. Learned
    // from a real frame rather than predicted, so an 8K plate gets 2 and a
    // 1080p clip gets the count.
    int playbackQueueDepthForMedia() const;
    // Begin/end prefetching for a run. Starting is refused while a shuttle run,
    // a drag or a storage read owns the decoder -- and it tests `scrubbing_`,
    // not isVideoScrubActive(), which means "the media is a video file".
    void startPlaybackPrefetch();
    // Drop the queue and the lookahead position. Called from reclaimDecoder(),
    // which is the single choke point every transition already funnels through.
    void stopPlaybackPrefetch();
    // Keep the pipeline full: post the next sequential frame while the queue is
    // under depth and nothing is in flight. Chained from onScrubResult.
    void pumpPlaybackQueue();
    // Answer this slot's target from the queue if it can be answered. Discards
    // entries the target has moved past (an audio catch-up or a real-time drop)
    // and re-seeds if the target has jumped clear of everything held. Returns
    // false to mean "starve": the caller HOLDS and must not decode.
    bool presentQueuedPlaybackFrame(long long targetFrame);
    // Bytes currently held, recomputed rather than tracked incrementally so it
    // cannot drift away from the deque it describes.
    long long playbackQueueBytes() const;

    // What endShuttleRun's landing branch costs THE UI THREAD, measured rather
    // than reasoned about. Added at spec phase 4 to settle `landPreviousExactly`
    // -- the branch is a synchronous Step decode of the frame already on screen,
    // and on a long-GOP file that is a seek plus a GOP walk. It sits BEFORE
    // beginPlaybackTimeline() in startShuttle, so the new run's own cadence
    // counters are reset after it and cannot see it: without this field the
    // landing measures free on every instrument the app already had.
    long long shuttleLandCount_ = 0;
    double shuttleLandLastMs_ = 0.0;
    double shuttleLandMaxMs_ = 0.0;

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
    // Whether the transport bar is in the layout (TRACE_TRANSPORT_BAR) or the
    // floating overlay is the transport. The bar object exists in both cases,
    // because it owns timelineSlider_ and therefore the whole scrub path.
    bool barIsDocked_ = false;
    // The spec's temporary shuttle-rate indicator, held here rather than in a
    // widget so the docked bar's label and the floating overlay's text are one
    // value. Cleared by rateFlashTimer_.
    QString rateFlashText_;
    QTimer rateFlashTimer_;
    // Geometry to restore when leaving fullscreen, and whether the window was
    // maximized when it entered. Fullscreen and maximize are different states
    // and the spec says not to confuse them.
    QByteArray preFullscreenGeometry_;
    bool preFullscreenMaximized_ = false;
    // Both exact-frame-step commands still exist and are still shared -- they
    // are reached by the Left and Right arrows alone from spec phases 5 and 4
    // respectively. The spec's "frame stepping becomes keyboard-only" removes
    // the BUTTONS, not the commands: "do not delete the underlying
    // exact-frame-step commands".
    QAction* prevFrameAction_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* nextFrameAction_ = nullptr;
    // What the old Next Frame and Prev Frame buttons became, at spec phases 4
    // and 5. Separate actions from the stepping pair rather than re-pointed
    // ones, because all four survive: two are controls on screen, two are
    // keyboard commands, and they now do entirely different things.
    QAction* fastForwardAction_ = nullptr;
    QAction* rewindAction_ = nullptr;
    // Both created in setupSharedActions(), which runs BEFORE setupMenus():
    // the menu adds the action, it does not define it. Before spec phase 2 the
    // fullscreen toggle was four lines written twice -- once for the menu and
    // once for the transport button -- with no action at all, which is the one
    // place the spec's "menus, buttons, tooltips, shortcuts and checked states
    // stay synchronized" requirement was not already met.
    // The source's start timecode, parsed once at open rather than per HUD
    // refresh. `hasSourceTimecode_` is the single gate on everything SMPTE:
    // the readout mode, the menu item and Go to Timecode all ask it, so
    // "the source has none" cannot be true in one place and false in another.
    bool hasSourceTimecode_ = false;
    bool sourceTimecodeDropFrame_ = false;
    long long sourceTimecodeStartFrames_ = 0;
    // The rate the timecode arithmetic runs at, exact. Timecode counts whole
    // frames per second, so 30000/1001 needs the rational and not the double --
    // this is the same reason fpsNum/fpsDen exist for the scheduler.
    int timecodeFpsNum_ = 24;
    int timecodeFpsDen_ = 1;

    QAction* timeDisplayFrameAction_ = nullptr;
    QAction* timeDisplaySecondsAction_ = nullptr;
    QAction* timeDisplayElapsedAction_ = nullptr;
    QAction* timeDisplayTimecodeAction_ = nullptr;
    QAction* goToFrameAction_ = nullptr;
    QAction* goToTimecodeAction_ = nullptr;

    // Share (spec phase 8). The gate is computed at open and read from here by
    // every surface, so "can this file be shared, and how" has one answer.
    trace::app::ShareState shareState_;
    // The integration's answer for the media currently open, and the path it was
    // asked about. The path is kept so a result that lands after the user has
    // opened something else is discarded rather than applied to the wrong file --
    // the same rule `requestGeneration_` enforces for frames.
    trace::app::LucidIntegrationState lucidState_;
    QString lucidProbePath_;
    bool lucidCopyInFlight_ = false;
    QAction* copyFilePathAction_ = nullptr;
    QAction* showInExplorerAction_ = nullptr;
    // Present, visible and unavailable until spec phase 9 builds the
    // integration. The design's state semantics require the row to be shown and
    // to say why, never hidden -- a command that comes and goes reads as a
    // broken build rather than as an answer about the file.
    QAction* copyLucidLinkAction_ = nullptr;
    QMenu* shareMenu_ = nullptr;

    // View transforms (spec phase 10). Temporary VIEWING state: they change no
    // pixel of the source, no frame identity and no timing, and they reset when
    // media changes.
    QAction* rotateLeftAction_ = nullptr;
    QAction* rotateRightAction_ = nullptr;
    QAction* flipHorizontalAction_ = nullptr;
    QAction* flipVerticalAction_ = nullptr;
    QAction* resetViewTransformAction_ = nullptr;

    // Spec phase 15's view scaling. Four commands over one piece of state,
    // which lives in ViewerWidget beside the transform.
    QAction* actualSizeAction_ = nullptr;
    QAction* fitToWindowAction_ = nullptr;
    QAction* zoomInAction_ = nullptr;
    QAction* zoomOutAction_ = nullptr;

    QAction* fullscreenAction_ = nullptr;
    // Escape, as a second surface onto fullscreenAction_. Its ENABLED state is
    // the "only while fullscreen" rule: a disabled QAction does not consume its
    // shortcut, so Escape passes through untouched when windowed.
    QAction* exitFullscreenAction_ = nullptr;
    QAction* toggleHudAction_ = nullptr;
    QAction* openAction_ = nullptr;

    // --- spec phase 14 -------------------------------------------------------
    // The window commands the spec's Window menu names. Minimize and
    // Maximize/Restore are one Qt call each; they are QActions rather than
    // inline lambdas for the reason phase 2 established when fullscreen stopped
    // being four duplicated lines -- a menu ADDS actions and never defines them,
    // and the Keyboard Shortcuts window renders whatever carries a shortcut.
    QAction* minimizeAction_ = nullptr;
    QAction* maximizeRestoreAction_ = nullptr;

    // Always on Top. Checkable, persisted through trace::app::settings().
    //
    // IMPLEMENTED WITH SetWindowPos, NOT setWindowFlag, AND THE DIFFERENCE IS
    // NOT COSMETIC. Changing a top-level widget's window flags makes Qt destroy
    // and recreate the native window -- and the D3D11 swapchain's surface is a
    // child HWND created once in initialize() from the viewer's winId(), so
    // recreating the parent orphans it. SetWindowPos(HWND_TOPMOST) changes the
    // z-order band and nothing about the window's identity.
    QAction* alwaysOnTopAction_ = nullptr;

    // Loop, and the Playback Speed group. Speed is a menu over the ONE rate
    // machine -- every item calls setPlaybackSpeed, which routes through
    // startShuttle or the half-speed path, and the checked item is read back
    // from playback_.state().speed rather than remembered here. Two sources for
    // "what rate is in force" is exactly what the spec's shared-actions rule
    // exists to prevent.
    QAction* loopAction_ = nullptr;
    QActionGroup* speedGroup_ = nullptr;
    std::vector<QAction*> speedActions_;

    QAction* closeMediaAction_ = nullptr;
    QAction* copyFrameAction_ = nullptr;

    // Help. Keyboard Shortcuts renders ShortcutTable::rows() and nothing is
    // written by hand -- that is the whole reason phase 3 built the table.
    QAction* keyboardShortcutsAction_ = nullptr;
    QAction* traceHelpAction_ = nullptr;
    QAction* reportIssueAction_ = nullptr;
    QAction* aboutAction_ = nullptr;
    // Created lazily and reused, like the inspector: modeless, so a second
    // invocation raises the existing window rather than stacking another.
    trace::app::ShortcutsWindow* shortcutsWindow_ = nullptr;

    // The accessibility proxy tree over the composited transport (plan section
    // 19.7). Null in TRACE_TRANSPORT_BAR=1 mode, where the transport is real
    // widgets that Qt already exposes. Parented to the viewer, so it is
    // destroyed with it.
    trace::app::OverlayAccessibility* overlayAccessibility_ = nullptr;

    // Movie Inspector (spec phase 13). Created lazily; null until first shown.
    trace::app::MovieInspector* inspector_ = nullptr;
    QAction* inspectorAction_ = nullptr;
    // WHY A TIMER, WHEN THE RULE IS "THE DIALOG READS, IT DOES NOT ASK".
    //
    // It still only reads -- this decides WHEN, and it exists because of phase
    // 10's trap. `RenderStats::lastDrawSize` is measured BY the paint, so a
    // panel refreshed at the instant the window changed reports the previous
    // viewport, and a paused file never refreshes again to correct it. The
    // obvious fix -- refresh from resizeEvent -- is the one the phase 12 record
    // rules out for the HUD, because building the string on ~123 events per
    // drag puts the instrument inside the path being measured.
    //
    // A single-shot armed by every event that can change what the panel shows
    // answers both: arming costs a timer restart, and by the time it fires the
    // paint has happened, so the size it reads is the size actually drawn. It
    // is not a poll -- it is never armed while the window is hidden, and it
    // fires once per settled change rather than on a schedule.
    QTimer inspectorRefreshTimer_;
    // Long enough that a whole interactive resize collapses into one refresh
    // and the paint that follows it has certainly landed; short enough that the
    // panel is not visibly stale after a gesture ends.
    static constexpr int kInspectorRefreshMs = 150;
    // Long enough that Qt has certainly applied the new scale factor and relaid
    // out -- the shaping pass measures chrome from what the layout DID, so a
    // pass run against the old layout would shape from stale terms and then
    // converge to the wrong answer rather than failing visibly.
    static constexpr int kDpiReshapeMs = 200;
    // Size of the file currently open, for media that does NOT go through
    // VideoDecoderFFmpeg -- a still image or a sequence frame. Video takes it
    // from VideoPerfStats::sourceBytes, which MediaIoSource already read as part
    // of opening the file.
    //
    // Taken ONCE, in openPath, where the path is being touched anyway. Never
    // when the inspector is shown: an unreachable UNC path costs 21,037ms to
    // stat on this box and the spec forbids blocking to compute optional values.
    qint64 openedFileBytes_ = -1;

    // Open Recent (spec phase 11). The list is the model and the submenu is
    // rebuilt from it; the actions inside are owned by the submenu and replaced
    // wholesale, so nothing here can hold a stale path.
    trace::app::RecentFiles recentFiles_;
    QMenu* recentMenu_ = nullptr;
    QAction* clearRecentAction_ = nullptr;

    // Trace's complete keyboard contract, and the dispatcher for the half of it
    // keyPressEvent still owns. Spec phase 13 renders the Keyboard Shortcuts
    // window from rows() rather than from a second list written by hand.
    trace::app::ShortcutTable shortcuts_;
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
    // The frame that was on screen when the timeline was epoched. Media time is
    // `presentAnchorFrame_ + (now - presentEpochNs_) / presentPeriodNs_`, which is
    // the frame that OUGHT to be showing at this instant -- the clock the
    // real-time drop is driven from. Without an anchor the epoch says when a run
    // started but not where, and Play from the middle of a file would map media
    // time onto the wrong frame.
    long long presentAnchorFrame_ = 0;
    // Real-time frame dropping (owner decision, 2026-08-13): a source that cannot
    // sustain its native rate holds MEDIA TIME real-time and drops picture,
    // rather than playing the whole movie slowly.
    //
    // `playbackDroppedFrames_` counts frames never presented; `playbackDropTicks_`
    // counts presents that dropped anything, so the two together say whether the
    // dropping is a steady light trickle or occasional lumps; `maxDropRun_` is the
    // worst single gap. All three are zero on every source that keeps up, which is
    // what makes "engages only when required" checkable rather than asserted.
    long long playbackDroppedFrames_ = 0;
    long long playbackDropTicks_ = 0;
    long long maxDropRun_ = 0;
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
    // How many consecutive frames one asynchronous request should cover. Takes
    // the stride as an argument rather than recomputing it, because the two
    // must be decided together: a stride above 1 forces this to 1.
    long long computeScrubBatch(long long gap, long long stride) const;
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

    // ---- The exact landing, off the UI thread ----------------------------
    //
    // A landing is one frame, decoded through RequestMode::Step at full
    // resolution with the accurate conversion, and it is the frame the user
    // stopped on. That contract is unchanged; what changed is which thread pays
    // for it. On a file whose every random-access miss walks from the head --
    // the 97-frame single-GOP Seedance clip is the measured case -- the walk is
    // 261-585ms, and inside the mouse handler that is a dead window rather than
    // a slow one.
    //
    // Which gesture is outstanding matters, because each has a different thing
    // to do once the frame is on screen: a release restores the play intent, a
    // step reverts the playhead if the decode failed, a press does neither.
    enum class LandingKind { None, ScrubPress, ScrubRelease, Step };
    // Post the exact frame to the worker. Returns true if it was posted, in
    // which case the caller returns and the completion runs from
    // onScrubResult(); false means the synchronous landing must be taken, which
    // is every non-video path, a storage-busy re-entry, and TRACE_ASYNC_LANDING=0.
    bool requestExactFrameAsync(long long frame, int direction, LandingKind kind);
    // Present the landed frame and run whatever the gesture owed. Called from
    // the onScrubResult drain loop; the reclaim and the completion action run
    // after the loop, because reclaimDecoder() clears the result queue it is
    // iterating.
    void completeExactLanding(const trace::core::ScrubResult& result);
    bool landingPending_ = false;
    long long landingFrame_ = -1;
    long long landingGeneration_ = -1;
    LandingKind landingKind_ = LandingKind::None;
    // What stepOneFrame passed, so a failed landing can put the playhead back
    // the way the synchronous path did. Zero for the scrub landings, which do
    // not revert.
    int landingStepDelta_ = 0;
    const char* landingHudLabel_ = "Scrub";
    QElapsedTimer landingClock_;
    // Wall-clock from posting the landing to the frame being on screen. This is
    // the same QUANTITY the synchronous path reported as `release` -- how long
    // until the exact frame appeared -- so the two remain comparable. What is no
    // longer the same is that it is not UI-thread occupancy: read `ui gap max`
    // for that, which is where this change is supposed to show.
    double landingLatencyMs_ = 0.0;
    double landingLatencyMaxMs_ = 0.0;
    long long landingsAsync_ = 0;
    long long landingsSync_ = 0;
    // Landings posted and then superseded before they arrived. Not an error --
    // it is the user moving on -- but a non-zero count on a gesture that should
    // have settled is how a chain that never completes would show.
    long long landingsSuperseded_ = 0;

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

    // Spec phase 12's first experiment: what an interactive resize actually
    // costs. Section 2 item 7 predicted a cache thrash under continuous
    // aspect-locked drag-resizing and named two independent costs; these count
    // both of them separately, because the prediction is about the second and
    // the first is the one that runs on every event.
    //
    // `resizeEvents_` is every resizeEvent; `previewSizeChanges_` is how many of
    // those reached a real size change inside the decoder; `cacheEntriesDropped_`
    // is how many entries those changes actually discarded. The three are kept
    // apart on purpose -- a clear of an empty cache is free, so the number of
    // clears is not the cost and only the entry count is.
    long long resizeEvents_ = 0;
    long long previewSizeChanges_ = 0;
    long long cacheEntriesDropped_ = 0;
    double syncPreviewMsTotal_ = 0.0;
    double syncPreviewMsMax_ = 0.0;
    // Kept here so the change can be COUNTED without moving the comparison that
    // the decoder already makes. Section 2 item 7 also proposes hoisting that
    // comparison above reclaimDecoder(); this is deliberately not that change --
    // the first experiment has to measure what ships.
    QSize lastPreviewPx_;
    // The three Win32 messages the aspect lock will be built on, counted before
    // anything depends on them. WM_SIZING per drag says how often a constraint
    // would be applied; the ENTERSIZEMOVE/EXITSIZEMOVE pair says whether the
    // brackets that would replace a debounce timer actually arrive.
    long long wmSizing_ = 0;
    long long wmEnterSizeMove_ = 0;
    long long wmExitSizeMove_ = 0;
    // The control on the three above -- see the switch in nativeEvent().
    long long wmSize_ = 0;
    // Section 20.4. This message has never been received by this application:
    // the box had one display until 2026-08-14, so every devicePixelRatioF()
    // term in the shipping path had only ever been the identity, and the
    // --window-shape-selftest that covers the arithmetic drives `dpr` as an
    // ARGUMENT with no window, renderer or display behind it.
    //
    // It is COUNTED rather than inferred from a changed `win WxH`, because the
    // two are not the same claim: a window can change device size because it
    // moved to a monitor with a different scale factor, or because something
    // resized it, and from a screenshot those are identical. `dpiChg` moving is
    // the only evidence the per-monitor DPI path executed at all.
    long long wmDpiChanged_ = 0;
    // The dpr in force when the last DPI change was handled, so a run can say
    // which direction it went rather than only that it happened.
    double lastDpiChangeFrom_ = 0.0;
    double lastDpiChangeTo_ = 0.0;
    // Section 4: "recalculate correctly when the window moves between monitors
    // with different scaling". MEASURED on hardware 2026-08-14, and it did not:
    // a DPI change is not a WindowStateChange and sends no WM_SIZING, so
    // neither changeEvent's re-shape nor constrainSizingRect's aspect lock ever
    // ran, and the window came off the crossing the wrong shape with the
    // picture pillarboxed inside it.
    //
    // COALESCED, and it has to be. At WM_DPICHANGED time Qt has not yet applied
    // the new scale factor or relaid out, so shaping there would read the OLD
    // dpr and the OLD chrome -- the same stale-input mistake as shaping before
    // refreshHud at open, which pillarboxed the 4x5. One crossing can also
    // produce more than one message.
    QTimer dpiReshapeTimer_;
    // The scale factor the window was last SHAPED at, which is the thing the
    // reshape is conditional on -- not the monitor. Two monitors at the same
    // scale factor need no reshape, and resizing the window on a move between
    // them would be section 4's "do not move the window unexpectedly" in
    // spirit: the user moved it, they did not ask for it to be resized.
    double lastShapeDpr_ = 0.0;
    // Reshapes actually performed, so the HUD can distinguish "the message
    // arrived" from "something was done about it".
    long long dpiReshapes_ = 0;
    // View > Lock Window to Media Aspect Ratio (spec section 4), checked by
    // default and persisted through trace::app::settings() -- the home phase 11
    // built, which this is the second consumer of and must not become a second
    // one of.
    QAction* lockAspectAction_ = nullptr;
    static constexpr const char* kLockAspectKey = "view/lockWindowToMediaAspect";
    // Spec phase 14, through the same one home. Both are review preferences
    // rather than properties of the media, so both survive a file change.
    static constexpr const char* kAlwaysOnTopKey = "view/alwaysOnTop";
    static constexpr const char* kLoopKey = "playback/loop";
    // Where Report an Issue sends mail. Owner decision, 2026-08-11: a mailto
    // rather than the repository, which is private.
    static constexpr const char* kIssueEmail = "bigsbypuglise@gmail.com";
    // Mirrors loopAction_ so the playback tick reads a bool rather than
    // dereferencing a QAction on the hot path -- and so the tick keeps working
    // if the action is ever created later than the timer.
    bool loopEnabled_ = false;
    // How many times the current session has wrapped. On the HUD, because a
    // wrap re-establishes the playback timeline and therefore RESETS every
    // cadence counter beside it -- a figure quoted from a looping run is one
    // lap's, and this is what says so.
    long long loopWraps_ = 0;
    // The outer rect at WM_ENTERSIZEMOVE, in physical pixels. A corner drag
    // moves both axes at once and the constraint has to pick one to honour;
    // comparing against where the drag STARTED is what makes that choice stable
    // for the whole gesture, where comparing against the previous proposed rect
    // would let it flip axis mid-drag and read as oscillation.
    QRect sizeMoveStartRect_;
    bool inSizeMove_ = false;

    std::optional<trace::core::MediaItem> currentMedia_;
    std::optional<trace::core::LoadedImageInfo> currentImage_;
    // The video frame most recently pulled from the decoder. Held across
    // requests so the decoder's conversion pool sees a steady number of
    // outstanding references rather than one that collapses between frames.
    trace::core::VideoFrame videoFrameBuffer_;
};

} // namespace trace::app
