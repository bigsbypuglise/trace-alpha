#include "app/MainWindow.h"

#include <QAction>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMenuBar>
#include <QMimeData>
#include <QStatusBar>

#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

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

MainWindow::MainWindow() {
    setWindowTitle("Trace");
    setAcceptDrops(true);
    setupUi();
    setupMenus();
    setupTransportControls();

    connect(&playTimer_, &QTimer::timeout, this, [this]() {
        if (!frameSource_ || !frameSource_->canPlay()) return;

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
        });

        const auto playbackState = playback_.state();
        if (playbackState.mode != PlaybackMode::PlayingForward && playbackState.mode != PlaybackMode::PlayingReverse) {
            playTimer_.stop();
            playbackClock_.invalidate();
            playbackAccumulatorMs_ = 0.0;
            return;
        }

        const int direction = playbackState.mode == PlaybackMode::PlayingReverse ? -1 : 1;
        const double speed = std::max(1.0, std::abs(playbackState.speed));
        const double fps = std::max(1.0, frameSource_->fps());
        const double frameDurationMs = 1000.0 / (fps * speed);

        if (!playbackClock_.isValid()) {
            playbackClock_.start();
            playbackAccumulatorMs_ = 0.0;
        } else {
            playbackAccumulatorMs_ += static_cast<double>(playbackClock_.restart());
        }

        const bool isVideo = currentMedia_.has_value() && currentMedia_->kind == MediaKind::VideoFile;

        // Timer jitter: how far this tick landed from the requested interval.
        // Sampled before any presentation gating so it measures the scheduler
        // itself rather than the decision made from it.
        if (isVideo) {
            if (!schedulerTickClock_.isValid()) {
                schedulerTickClock_.start();
            } else {
                const double tickDeltaMs = static_cast<double>(schedulerTickClock_.restart());
                lastTickJitterMs_ = tickDeltaMs - static_cast<double>(kSchedulerTickMs);
                const double absJitter = std::abs(lastTickJitterMs_);
                ++schedulerTicks_;
                avgTickJitterMs_ += (absJitter - avgTickJitterMs_) / static_cast<double>(schedulerTicks_);
                maxTickJitterMs_ = std::max(maxTickJitterMs_, absJitter);
            }
        }

        int steps = static_cast<int>(std::floor(playbackAccumulatorMs_ / frameDurationMs));
        if (steps < 1) steps = 1;

        if (isVideo) {
            // The short tick exists to land on each frame's due time, not to
            // present once per tick. Presenting per tick made the timer
            // interval the playback rate: 1000/24 rounds to a 42ms interval,
            // capping playback at 23.81fps no matter how fast decode is.
            // Retained as a guard: with the periodic timer at the frame
            // interval this is effectively never taken, but it keeps
            // presentation tied to the playback clock rather than to the tick.
            if (playbackAccumulatorMs_ < frameDurationMs) return;

            // Presentation latency: how far past its due time this frame went.
            lastPresentLatencyMs_ = playbackAccumulatorMs_ - frameDurationMs;
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
        const long long unclampedTarget = beforeFrame + static_cast<long long>(direction) * steps;
        const long long minFrame = 0;
        const long long maxFrame = playbackState.maxFrame >= 0 ? playbackState.maxFrame : beforeFrame;
        const long long targetFrame = std::clamp(unclampedTarget, minFrame, maxFrame);
        playback_.setCurrentFrame(targetFrame);

        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, direction);
        QString error;
        if (!loadCurrentFrame(error, trace::core::VideoDecoderFFmpeg::RequestMode::Playback)) {
            playback_.setCurrentFrame(beforeFrame);
            playTimer_.stop();
            playback_.pause();
            playbackClock_.invalidate();
            playbackAccumulatorMs_ = 0.0;
            if (!error.isEmpty()) statusBar()->showMessage(error, 2000);
        } else {
            ++playbackFramesPresented_;
            playbackRunElapsedS_ = static_cast<double>(playbackRateClock_.elapsed()) / 1000.0;
            // First/last present span: N presented frames cover N-1 intervals,
            // so rate from this span is the honest steady-state figure.
            const qint64 nowNs = sessionClock_.isValid() ? sessionClock_.nsecsElapsed() : 0;
            if (firstPresentNs_ < 0) firstPresentNs_ = nowNs;
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
            playback_.pause();
            playbackClock_.invalidate();
            playbackAccumulatorMs_ = 0.0;
        }
        refreshHud(direction > 0 ? "Play" : "Reverse Play");

    });

    scrubTimer_.setSingleShot(true);
    scrubTimer_.setInterval(12);
    connect(&scrubTimer_, &QTimer::timeout, this, [this]() {
        flushVideoScrub(false);
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
    connect(timelineSlider_, &QSlider::sliderPressed, this, [this]() {
        if (suppressSliderSignal_) return;
        scrubbing_ = true;
        playback_.pause();
        playTimer_.stop();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;
    });

    connect(timelineSlider_, &QSlider::sliderReleased, this, [this]() {
        if (suppressSliderSignal_) return;
        scrubbing_ = false;

        if (isVideoScrubActive()) {
            queueVideoScrubFrame(static_cast<long long>(timelineSlider_->value()));
            flushVideoScrub(true);
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
        refreshHud("Scrub");
    });

    connect(timelineSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (suppressSliderSignal_) return;
        playback_.pause();
        playTimer_.stop();
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

void MainWindow::syncTransportBar() {
    if (!timelineSlider_ || !playPauseAction_) return;

    const auto st = playback_.state();
    const int maxFrame = static_cast<int>(std::max(0LL, st.maxFrame));

    suppressSliderSignal_ = true;
    timelineSlider_->setMaximum(maxFrame);
    timelineSlider_->setValue(static_cast<int>(std::clamp(st.currentFrame, 0LL, st.maxFrame < 0 ? 0LL : st.maxFrame)));
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

void MainWindow::openPath(const QString& path) {
    playTimer_.stop();
    scrubTimer_.stop();
    scrubbing_ = false;
    pendingScrubFrame_ = -1;
    activeScrubFrame_ = -1;
    playbackClock_.invalidate();
    playbackAccumulatorMs_ = 0.0;
    videoDecoder_.close();
    frameSource_.reset();
    videoFrameBuffer_ = QImage();
    lastFrameHandoffMs_ = 0.0;
    avgFrameHandoffMs_ = 0.0;
    frameHandoffSamples_ = 0;

    trace::core::MediaItem item;
    item.path = path.toStdString();

    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();

    if (ext == "mp4" || ext == "mov") {
        QString err;
        if (videoDecoder_.open(path, err)) {
            item.kind = MediaKind::VideoFile;
            item.frameCount = videoDecoder_.metadata().frameCount;
            frameSource_ = std::make_unique<trace::core::VideoFrameSource>(&videoDecoder_);
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
    playTimer_.setSingleShot(false);
    playTimer_.setInterval(static_cast<int>(std::round(1000.0 / fps)));
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
            info.image = cached->image;
            currentImage_ = info;
            viewer_->setImage(info.image);
            syncTransportBar();
            return true;
        }
    }

    QImage decodedImage;
    QImage* targetImage = &decodedImage;

    if (currentMedia_->kind == MediaKind::VideoFile) {
        targetImage = &videoFrameBuffer_;
    }

    if (!frameSource_->frameAt(frameIndex, *targetImage, error)) return false;

    const QString sourcePath = frameSource_->sourcePathForFrame(frameIndex).isEmpty()
        ? QString::fromStdString(currentMedia_->path)
        : frameSource_->sourcePathForFrame(frameIndex);

    QElapsedTimer handoffTimer;
    handoffTimer.start();

    trace::core::LoadedImageInfo info;
    info.filePath = sourcePath;
    info.fileName = QFileInfo(sourcePath).fileName();
    info.extension = QFileInfo(sourcePath).suffix().toLower();
    info.width = targetImage->width();
    info.height = targetImage->height();
    info.channels = 4;

    if (currentMedia_->kind == MediaKind::VideoFile) {
        currentImage_ = info;
        viewer_->setImage(*targetImage);
    } else {
        info.image = *targetImage;
        currentImage_ = info;
        viewer_->setImage(info.image);
    }

    lastFrameHandoffMs_ = static_cast<double>(handoffTimer.nsecsElapsed()) / 1'000'000.0;
    videoDecoder_.setHandoffTiming(lastFrameHandoffMs_);
    ++frameHandoffSamples_;
    const double handoffN = static_cast<double>(frameHandoffSamples_);
    avgFrameHandoffMs_ += (lastFrameHandoffMs_ - avgFrameHandoffMs_) / handoffN;

    if (currentMedia_->kind == MediaKind::ImageSequence) {
        trace::core::CachedFrame cf;
        cf.frameIndex = frameIndex;
        cf.path = info.filePath;
        cf.image = info.image;
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
        cf.image = info.image;
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
        playback_.pause();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;
    } else {
        playback_.togglePlayPause();
        const int direction = playback_.state().mode == PlaybackMode::PlayingReverse ? -1 : 1;
        prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, direction, false);
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
        playTimer_.start();
    }
    syncTransportBar();
}

bool MainWindow::isVideoScrubActive() const {
    return currentMedia_.has_value() && currentMedia_->kind == MediaKind::VideoFile;
}

void MainWindow::queueVideoScrubFrame(long long frameIndex) {
    pendingScrubFrame_ = frameIndex;
    playback_.setCurrentFrame(frameIndex);

    // If a coalescing window is open, the timer picks up the latest pending
    // frame when it fires. Otherwise respond immediately (snappy first frame),
    // then open a window so rapid slider moves coalesce instead of stacking
    // one synchronous decode per event.
    if (scrubTimer_.isActive()) return;
    flushVideoScrub(false);
    scrubTimer_.start();
}

void MainWindow::flushVideoScrub(bool forceExact) {
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

    activeScrubFrame_ = targetFrame;
    playback_.setCurrentFrame(targetFrame);

    // Mid-drag frames use Scrub (fast, half-res preview at 4K). The landing
    // frame — slider release or a jump while not dragging — uses Step so the
    // frame being inspected is full-res and accurately converted.
    const auto mode = (forceExact || !scrubbing_)
        ? trace::core::VideoDecoderFFmpeg::RequestMode::Step
        : trace::core::VideoDecoderFFmpeg::RequestMode::Scrub;
    prepareVideoRequest(mode, 1, true);
    QString error;
    if (!loadCurrentFrame(error, mode)) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
    }

    if (forceExact || !scrubbing_) {
        pendingScrubFrame_ = -1;
    } else if (pendingScrubFrame_ != activeScrubFrame_) {
        scrubTimer_.start();
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
            const auto& vm = videoDecoder_.metadata();
            const auto& perf = videoDecoder_.perfStats();
            const auto& drawPerf = viewer_->perfStats();
            const double reverseHitRate = perf.reverseCacheLookups > 0
                ? (100.0 * static_cast<double>(perf.reverseCacheHits) / static_cast<double>(perf.reverseCacheLookups))
                : 0.0;

            // Three grouped lines: a single line overflowed the window at 4K
            // and clipped exactly the fields needed to diagnose playback
            // (rev-hit, late, walk).
            const double budgetMs = vm.fps > 0.0 ? 1000.0 / vm.fps : 41.67;
            const double lastTotalMs = perf.lastDecodeMs + perf.lastConvertMs
                                     + perf.lastConvertWrapMs + perf.lastConvertAllocMs
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

            const QString l2 = QString("dec %1/%2 | sws %3/%4 | ctx %5/%6 | detach %7/%8 | alloc %9 | memcpy %10 | handoff %11 | draw %12 | total %13 | budget %14ms")
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
                .arg(QString::number(budgetMs, 'f', 2));

            const QString l3 = QString("cvt/req %1 | ctx-rebuilds %2 | shared %3 | sws %4 | %5 | rev-hit %6%% (%7/%8) | late %9 | walk %10f cache %11cv/%12ms | seek %13/%14 n=%15 | drain %16pk/%17f stale-blocked %18")
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
                .arg(perf.staleSuccessPrevented);

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

            const QString l5 = QString("sched tick %1ms | jitter %2/%3/%4 (last/avg/max) | present-late %5/%6/%7 | drift %8ms | ticks %9 | presents %10")
                .arg(kSchedulerTickMs)
                .arg(QString::number(lastTickJitterMs_, 'f', 2))
                .arg(QString::number(avgTickJitterMs_, 'f', 2))
                .arg(QString::number(maxTickJitterMs_, 'f', 2))
                .arg(QString::number(lastPresentLatencyMs_, 'f', 2))
                .arg(QString::number(avgPresentLatencyMs_, 'f', 2))
                .arg(QString::number(maxPresentLatencyMs_, 'f', 2))
                .arg(QString::number(lastDriftMs_, 'f', 1))
                .arg(schedulerTicks_)
                .arg(presentSamples_);

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

            line = l1 + "\n" + l2 + "\n" + l3 + "\n" + l4 + "\n" + l5 + "\n" + l6;
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
        case Qt::Key_Left:
            playback_.stepBackward();
            needsReload = true;
            prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, -1, true);
            if (playTimer_.isActive()) {
                playTimer_.stop();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            break;
        case Qt::Key_Right:
            playback_.stepForward();
            needsReload = true;
            prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Step, 1, false);
            if (playTimer_.isActive()) {
                playTimer_.stop();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            break;
        case Qt::Key_J:
            if (frameSource_ && frameSource_->canPlay()) {
                playback_.jogReverse();
                prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, -1, true);
                playbackClock_.start();
                playbackAccumulatorMs_ = 0.0;
                if (!playTimer_.isActive()) playTimer_.start();
            }
            refreshHud("J");
            return;
        case Qt::Key_K:
            playback_.pause();
            if (playTimer_.isActive()) {
                playTimer_.stop();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            refreshHud("K");
            return;
        case Qt::Key_L:
            if (frameSource_ && frameSource_->canPlay()) {
                playback_.jogForward();
                prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode::Playback, 1, false);
                playbackClock_.start();
                playbackAccumulatorMs_ = 0.0;
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
    auto* videoSource = videoFrameSource();
    if (!videoSource) return;
    videoSource->setRequestMode(mode);
    videoSource->setPlaybackDirection(direction);
    if (clearQueue) {
        videoSource->clearPlaybackQueue();
    }
}

} // namespace trace::app
