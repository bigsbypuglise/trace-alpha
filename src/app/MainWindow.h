#pragma once

#include <QMainWindow>
#include <QTimer>
#include <QElapsedTimer>
#include <optional>
#include "core/MediaItem.h"
#include "core/ViewState.h"
#include "core/PlaybackController.h"
#include "core/StillImageLoader.h"
#include "core/FrameCache.h"
#include "core/VideoDecoderFFmpeg.h"

QT_BEGIN_NAMESPACE
class QKeyEvent;
class QDragEnterEvent;
class QDropEvent;
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
    void openFileDialog();
    void openPath(const QString& path);
    bool loadCurrentFrame(QString& error);
    QString sequenceFramePath(long long frameIndex) const;
    void prefetchNeighbors();
    void togglePlayPauseVideo();
    void refreshHud(const QString& action = {});

    trace::ui::ViewerWidget* viewer_ = nullptr;
    trace::ui::TransportOverlay* overlay_ = nullptr;

    trace::core::PlaybackController playback_;
    trace::core::ViewState viewState_;
    trace::core::StillImageLoader stillLoader_;
    trace::core::FrameCache frameCache_{1};
    trace::core::VideoDecoderFFmpeg videoDecoder_;
    QTimer playTimer_;
    QElapsedTimer playbackClock_;
    double playbackAccumulatorMs_ = 0.0;

    std::optional<trace::core::MediaItem> currentMedia_;
    std::optional<trace::core::LoadedImageInfo> currentImage_;
    bool isVideo_ = false;
};

} // namespace trace::app
