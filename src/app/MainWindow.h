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

protected:
    void keyPressEvent(QKeyEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dropEvent(QDropEvent* event) override;

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
    bool isVideoScrubActive() const;
    void queueVideoScrubFrame(long long frameIndex);
    void flushVideoScrub(bool forceExact);
    trace::core::VideoFrameSource* videoFrameSource();
    void prepareVideoRequest(trace::core::VideoDecoderFFmpeg::RequestMode mode, int direction = 1, bool clearQueue = false);

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
    static constexpr int kSchedulerTickMs = 42;
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
