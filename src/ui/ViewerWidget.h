#pragma once

#include <QWidget>
#include <QImage>
#include <QSize>
#include <QString>
#include <QElapsedTimer>

#include "core/VideoFrame.h"

namespace trace::ui {

struct ViewerPerfStats {
    // Scope note: lastPaintMs historically covered the paintEvent body only,
    // stopping before ~QPainter (which flushes). The fields below separate the
    // stages so no single number gets mistaken for total presentation cost.
    double lastPaintMs = 0.0;
    double avgPaintMs = 0.0;
    long long samples = 0;

    double lastDrawImageMs = 0.0;   // QPainter::drawImage only
    double avgDrawImageMs = 0.0;
    double lastPaintTotalMs = 0.0;  // paintEvent entry -> after ~QPainter
    double avgPaintTotalMs = 0.0;
    double lastUpdateToPaintMs = 0.0; // update() request -> paintEvent entry
    double avgUpdateToPaintMs = 0.0;
    long long paintCount = 0;
    long long updateCount = 0;
    // Set when a paint is served without a new setImage() since the last one.
    long long paintsWithoutNewImage = 0;

    // Whether the last frame was resampled to fit, and the size it was drawn
    // at: the scale factor is what decides whether filtering is good enough or
    // the downscale needs to move into swscale.
    bool lastDrawWasScaled = false;
    bool lastDrawWasFiltered = false;
    QSize lastDrawSize;
};

class ViewerWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);

    void setFrame(const trace::core::VideoFrame& frame);
    void clearImage();
    void setCenterText(const QString& text);
    const ViewerPerfStats& perfStats() const { return perfStats_; }
    // What is currently on screen. The renderer boundary will want the frame
    // rather than the pixels; until then it is also the honest answer to "which
    // frame is displayed", which the HUD has had to infer.
    const trace::core::VideoFrame& frame() const { return frame_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    // The frame holds the buffer alive; image_ is a zero-copy read-only view
    // over it, built once per frame rather than per paint.
    trace::core::VideoFrame frame_;
    QImage image_;
    bool hasImage_ = false;
    QString centerText_ = "Drop media or File > Open";
    ViewerPerfStats perfStats_{};
    // Single monotonic source for this widget, so update()->paint latency is
    // measured against one clock rather than two independent timers.
    QElapsedTimer clock_;
    qint64 updateRequestedNs_ = -1;
};

} // namespace trace::ui
