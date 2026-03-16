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
#include <QToolBar>
#include <QSlider>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include "ui/ViewerWidget.h"
#include "ui/TransportOverlay.h"
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
    setupDeveloperTransportBar();

    connect(&playTimer_, &QTimer::timeout, this, [this]() {
        if (!frameSource_ || !frameSource_->canPlay()) return;

        const double fps = std::max(1.0, frameSource_->fps());
        const double frameDurationMs = 1000.0 / fps;

        if (!playbackClock_.isValid()) {
            playbackClock_.start();
            playbackAccumulatorMs_ = 0.0;
            requestPlaybackPrefetch();
            refreshHud("Play");
            return;
        }

        playbackAccumulatorMs_ += static_cast<double>(playbackClock_.restart());

        // Lightweight timing governor:
        // - wait if we're early
        // - present one frame on time
        // - skip late intermediate frames (count as dropped) to keep motion stable
        constexpr int kMaxAdvancePerTick = 4;
        const int dueFrames = static_cast<int>(std::floor(playbackAccumulatorMs_ / frameDurationMs));
        if (dueFrames < 1) {
            requestPlaybackPrefetch();
            refreshHud("Play");
            return;
        }

        const long long beforeFrame = playback_.state().currentFrame;
        const int advanceFrames = std::clamp(dueFrames, 1, kMaxAdvancePerTick);
        if (advanceFrames > 1) lateOrDroppedFrames_ += (advanceFrames - 1);

        playback_.setCurrentFrame(beforeFrame + advanceFrames);

        playbackAccumulatorMs_ -= static_cast<double>(advanceFrames) * frameDurationMs;
        if (playbackAccumulatorMs_ < 0.0) playbackAccumulatorMs_ = 0.0;

        QString error;
        const bool framePresented = loadCurrentFrame(error);
        if (!framePresented) {
            ++lateOrDroppedFrames_;
            if (!error.isEmpty()) statusBar()->showMessage(error, 1200);
        }

        updateAdaptivePrefetch(framePresented);
        requestPlaybackPrefetch();

        const auto st = playback_.state();
        if (st.currentFrame == beforeFrame && st.maxFrame >= 0 && st.currentFrame >= st.maxFrame) {
            playTimer_.stop();
            playback_.pause();
            playbackClock_.invalidate();
            playbackAccumulatorMs_ = 0.0;
        }
        refreshHud("Play");
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
    overlay_ = new trace::ui::TransportOverlay(central);

    layout->addWidget(viewer_, 1);
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
        refreshHud("Fullscreen");
    });
    fileMenu->addAction(fullscreenAction);

    fileMenu->addSeparator();
    auto* quitAction = new QAction("&Quit", this);
    quitAction->setShortcut(QKeySequence::Quit);
    connect(quitAction, &QAction::triggered, this, &QWidget::close);
    fileMenu->addAction(quitAction);
}

void MainWindow::setupDeveloperTransportBar() {
    devTransportBar_ = addToolBar("Developer Transport");
    devTransportBar_->setMovable(false);

    prevFrameAction_ = devTransportBar_->addAction("Previous Frame");
    connect(prevFrameAction_, &QAction::triggered, this, [this]() {
        playback_.stepBackward();
        playback_.pause();
        playTimer_.stop();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;

        QString error;
        if (!loadCurrentFrame(error)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
            playback_.stepForward();
            loadCurrentFrame(error);
        }
        requestPlaybackPrefetch();
        refreshHud("Prev Frame");
    });

    playPauseAction_ = devTransportBar_->addAction("Play");
    connect(playPauseAction_, &QAction::triggered, this, [this]() {
        togglePlayPause();
        refreshHud("Play/Pause");
    });

    nextFrameAction_ = devTransportBar_->addAction("Next Frame");
    connect(nextFrameAction_, &QAction::triggered, this, [this]() {
        playback_.stepForward();
        playback_.pause();
        playTimer_.stop();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;

        QString error;
        if (!loadCurrentFrame(error)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
            playback_.stepBackward();
            loadCurrentFrame(error);
        }
        requestPlaybackPrefetch();
        refreshHud("Next Frame");
    });

    timelineSlider_ = new QSlider(Qt::Horizontal, devTransportBar_);
    timelineSlider_->setMinimum(0);
    timelineSlider_->setMaximum(0);
    timelineSlider_->setValue(0);
    timelineSlider_->setMinimumWidth(220);
    connect(timelineSlider_, &QSlider::valueChanged, this, [this](int value) {
        if (suppressSliderSignal_) return;
        playback_.setCurrentFrame(static_cast<long long>(value));
        playback_.pause();
        playTimer_.stop();
        playbackClock_.invalidate();
        playbackAccumulatorMs_ = 0.0;

        QString error;
        if (!loadCurrentFrame(error)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
        }
        requestPlaybackPrefetch();
        refreshHud("Scrub");
    });
    devTransportBar_->addWidget(timelineSlider_);

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
}

void MainWindow::openFileDialog() {
    const QString filter = "Media (*.mp4 *.mov *.png *.jpg *.jpeg *.tif *.tiff *.exr);;All Files (*.*)";
    const QString path = QFileDialog::getOpenFileName(this, "Open Media", {}, filter);
    if (!path.isEmpty()) openPath(path);
}

void MainWindow::openPath(const QString& path) {
    playTimer_.stop();
    playbackClock_.invalidate();
    playbackAccumulatorMs_ = 0.0;
    videoDecoder_.close();
    playbackPrefetcher_.configureForSequence({});
    frameSource_.reset();
    lateOrDroppedFrames_ = 0;
    consecutiveLateTicks_ = 0;
    consecutiveOnTimeTicks_ = 0;
    basePrefetchLookahead_ = 6;
    prefetchLookahead_ = 6;
    maxPrefetchLookahead_ = 14;

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

            const QString codec = videoDecoder_.metadata().codecName.toLower();
            basePrefetchLookahead_ = codec.contains("prores") ? 8 : 6;
            prefetchLookahead_ = basePrefetchLookahead_;
            maxPrefetchLookahead_ = 12;

            QString prefetchErr;
            if (!playbackPrefetcher_.configureForVideo(path, prefetchErr) && !prefetchErr.isEmpty()) {
                statusBar()->showMessage(prefetchErr, 1800);
            }
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
            playbackPrefetcher_.configureForSequence(framePaths);

            const QString sequenceSuffix = QString::fromStdString(seq->suffix).toLower();
            const bool isExr = sequenceSuffix.endsWith(".exr");
            basePrefetchLookahead_ = isExr ? 12 : 8;
            prefetchLookahead_ = basePrefetchLookahead_;
            maxPrefetchLookahead_ = isExr ? 20 : 14;

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
            const QStringList stillPaths{path};
            frameSource_ = std::make_unique<trace::core::ImageSequenceFrameSource>(&stillLoader_, stillPaths, 24.0);
            playbackPrefetcher_.configureForSequence(stillPaths);
            basePrefetchLookahead_ = 2;
            prefetchLookahead_ = 2;
            maxPrefetchLookahead_ = 4;
            playback_.resetForNewMedia(0);
            playback_.setCurrentFrame(0);
        }
    }

    currentMedia_ = item;
    frameCache_.clear();
    frameCache_.setWindowCenter(playback_.state().currentFrame);

    QString error;
    if (!loadCurrentFrame(error)) {
        if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
        refreshHud("Open failed");
        return;
    }

    requestPlaybackPrefetch();

    const auto fps = frameSource_ ? std::max(1.0, frameSource_->fps()) : 24.0;
    playTimer_.setInterval(static_cast<int>(std::round(1000.0 / fps)));

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

bool MainWindow::presentFrame(long long frameIndex, const QImage& image, const QString& sourcePath, int channels) {
    if (image.isNull()) return false;

    trace::core::LoadedImageInfo info;
    info.filePath = sourcePath;
    info.fileName = QFileInfo(sourcePath).fileName();
    info.extension = QFileInfo(sourcePath).suffix().toLower();
    info.width = image.width();
    info.height = image.height();
    info.channels = channels;
    info.image = image;

    currentImage_ = info;
    viewer_->setImage(info.image);

    if (currentMedia_.has_value() && currentMedia_->kind == MediaKind::ImageSequence) {
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

bool MainWindow::loadCurrentFrame(QString& error) {
    error.clear();
    if (!currentMedia_.has_value() || !frameSource_) {
        error = "No media selected";
        return false;
    }

    const long long frameIndex = playback_.state().currentFrame;
    frameSource_->setCurrentFrame(frameIndex);

    if (currentMedia_->kind == MediaKind::ImageSequence) {
        frameCache_.setWindowCenter(frameIndex);
        if (const auto cached = frameCache_.get(frameIndex); cached.has_value()) {
            return presentFrame(frameIndex, cached->image, cached->path, cached->channels);
        }
    }

    if (const auto prefetched = playbackPrefetcher_.take(frameIndex); prefetched.has_value()) {
        const long long presentedFrame = prefetched->frameIndex >= 0 ? prefetched->frameIndex : frameIndex;
        if (currentMedia_->kind == MediaKind::VideoFile && presentedFrame != frameIndex) {
            playback_.setCurrentFrame(presentedFrame);
        }

        const QString path = prefetched->path.isEmpty()
            ? (frameSource_->sourcePathForFrame(presentedFrame).isEmpty() ? QString::fromStdString(currentMedia_->path) : frameSource_->sourcePathForFrame(presentedFrame))
            : prefetched->path;
        return presentFrame(presentedFrame, prefetched->image, path, prefetched->channels > 0 ? prefetched->channels : 4);
    }

    const bool activePlayback = playTimer_.isActive();
    if (activePlayback) {
        // Do not block presentation when playback is active.
        // Decode queue is responsible for preparing upcoming frames.
        playbackPrefetcher_.request(frameIndex);
        error = "Frame not ready";
        return false;
    }

    QImage img;
    if (!frameSource_->frameAt(frameIndex, img, error)) return false;

    const long long presentedFrame = std::max(0LL, frameSource_->currentFrame());
    if (currentMedia_->kind == MediaKind::VideoFile && presentedFrame != frameIndex) {
        playback_.setCurrentFrame(presentedFrame);
    }

    const QString sourcePath = frameSource_->sourcePathForFrame(presentedFrame).isEmpty()
        ? QString::fromStdString(currentMedia_->path)
        : frameSource_->sourcePathForFrame(presentedFrame);

    const int channels = (currentMedia_->kind == MediaKind::VideoFile) ? 3 : 4;
    return presentFrame(presentedFrame, img, sourcePath, channels);
}

void MainWindow::requestPlaybackPrefetch() {
    if (!currentMedia_.has_value() || !frameSource_) return;

    const long long current = playback_.state().currentFrame;
    const long long maxFrame = playback_.state().maxFrame;

    for (long long i = 0; i <= prefetchLookahead_; ++i) {
        const long long idx = current + i;
        if (maxFrame >= 0 && idx > maxFrame) break;
        playbackPrefetcher_.request(idx);
    }
}

void MainWindow::updateAdaptivePrefetch(bool framePresentedOnTime) {
    if (!playTimer_.isActive()) return;

    if (framePresentedOnTime) {
        ++consecutiveOnTimeTicks_;
        consecutiveLateTicks_ = 0;

        // If we stay stable for a while, ease back toward baseline.
        if (consecutiveOnTimeTicks_ >= 24 && prefetchLookahead_ > basePrefetchLookahead_) {
            --prefetchLookahead_;
            consecutiveOnTimeTicks_ = 0;
        }
        return;
    }

    ++consecutiveLateTicks_;
    consecutiveOnTimeTicks_ = 0;

    // If decode is repeatedly late, expand lookahead depth to absorb spikes.
    if (consecutiveLateTicks_ >= 2 && prefetchLookahead_ < maxPrefetchLookahead_) {
        ++prefetchLookahead_;
        consecutiveLateTicks_ = 0;
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
        playbackClock_.start();
        playbackAccumulatorMs_ = 0.0;
        requestPlaybackPrefetch();
        playTimer_.start();
    }
    syncTransportBar();
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
            line = QString("Video | %1 | %2x%3 | fps %4 | codec %5 | Frame: %6")
                .arg(QFileInfo(QString::fromStdString(currentMedia_->path)).fileName())
                .arg(vm.width)
                .arg(vm.height)
                .arg(QString::number(vm.fps, 'f', 3))
                .arg(vm.codecName)
                .arg(st.currentFrame);
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

    line += QString(" | q:%1 drop:%2 la:%3").arg(playbackPrefetcher_.queuedCount()).arg(lateOrDroppedFrames_).arg(prefetchLookahead_);

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
            if (playTimer_.isActive()) {
                playTimer_.stop();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            break;
        case Qt::Key_Right:
            playback_.stepForward();
            needsReload = true;
            if (playTimer_.isActive()) {
                playTimer_.stop();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            break;
        case Qt::Key_J: playback_.jogReverse(); playback_.pause(); refreshHud("J"); return;
        case Qt::Key_K:
            playback_.pause();
            if (playTimer_.isActive()) {
                playTimer_.stop();
                playbackClock_.invalidate();
                playbackAccumulatorMs_ = 0.0;
            }
            refreshHud("K");
            return;
        case Qt::Key_L: playback_.jogForward(); playback_.pause(); refreshHud("L"); return;
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
        if (!loadCurrentFrame(error)) {
            if (!error.isEmpty()) statusBar()->showMessage(error, 3000);
            if (event->key() == Qt::Key_Left) playback_.stepForward();
            else playback_.stepBackward();
        }
        requestPlaybackPrefetch();
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

} // namespace trace::app
