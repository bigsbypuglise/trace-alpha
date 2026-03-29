#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <QImage>
#include <QString>
#include <optional>
#include <memory>
#include "core/MediaItem.h"
#include "core/ViewState.h"
#include "core/PlaybackController.h"
#include "core/StillImageLoader.h"
#include "core/FrameCache.h"
#include "core/VideoDecoderFFmpeg.h"
#include "core/FrameSource.h"
#include "core/VideoFrameSource.h"

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QDragEnterEvent;
class QDropEvent;
class QToolBar;
class QSlider;
class QAction;
QT_END_NAMESPACE

namespace trace::ui {
class ViewerWidget;
class TransportOverlay;
}

namespace trace::app {

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    MainWindow();

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    void setupUi();
    void setupMenus();
    void setupDeveloperTransportBar();
    void syncTransportBar();
    void openFileDialog();
    void openPath(const QString& path);
    bool loadCurrentFrame(QString& error, trace::core::VideoDecoderFFmpeg::RequestMode mode = trace::core::VideoDecoderFFmpeg::RequestMode::Playback);
    QString sequenceFramePath(long long frameIndex) const;
    void prefetchNeighbors();
    void togglePlayPause();
    void refreshHud(const QString& action = {});
    bool isVideoScrubActive() const;
    void queueVideoScrubFrame(long long frameIndex);
    void flushVideoScrub(bool forceExact);
    trace::core::VideoFrameSource* videoFrameSource();
    void prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode mode, int direction = 1, bool clearQueue = false);

    trace::ui::ViewerWidget* viewer_ = nullptr;
    trace::ui::TransportOverlay* overlay_ = nullptr;
    QToolBar* devTransportBar_ = nullptr;
    QAction* prevFrameAction_ = nullptr;
    QAction* playPauseAction_ = nullptr;
    QAction* nextFrameAction_ = nullptr;
    QSlider* timelineSlider_ = nullptr;

    trace::core::PlaybackController playback_;
    trace::core::ViewState viewState_;
    trace::core::StillImageLoader stillLoader_;
    trace::core::FrameCache frameCache_{1};
    trace::core::VideoDecoderFFmpeg videoDecoder_;
    std::unique_ptr<trace::core::FrameSource> frameSource_;
    QTimer playTimer_;
    QTimer scrubTimer_;
    QElapsedTimer playbackClock_;
    double playbackAccumulatorMs_ = 0.0;
    bool suppressSliderSignal_ = false;
    bool scrubbing_ = false;
    long long pendingScrubFrame_ = -1;
    long long activeScrubFrame_ = -1;

    double lastFrameHandoffMs_ = 0.0;
    double avgFrameHandoffMs_ = 0.0;
    long long frameHandoffSamples_ = 0;

    std::optional<trace::core::MediaItem> currentMedia_;
    std::optional<trace::core::LoadedImageInfo> currentImage_;
    QImage videoFrameBuffer_;
};

} // namespace trace::app
