#pragma once

#include <QWidget>
#include <QSize>
#include <QString>
#include <QElapsedTimer>
#include <memory>

#include "core/VideoFrame.h"
#include "render/VideoRenderer.h"

namespace trace::ui {

struct ViewerPerfStats {
    // Scope note: lastPaintMs historically covered the paintEvent body only,
    // stopping before ~QPainter (which flushes). The fields below separate the
    // stages so no single number gets mistaken for total presentation cost.
    double lastPaintMs = 0.0;
    double avgPaintMs = 0.0;
    long long samples = 0;

    double lastDrawImageMs = 0.0;   // the blit only
    double avgDrawImageMs = 0.0;
    double lastPaintTotalMs = 0.0;  // paintEvent entry -> after the flush
    double avgPaintTotalMs = 0.0;
    double lastUpdateToPaintMs = 0.0; // update() request -> paintEvent entry
    double avgUpdateToPaintMs = 0.0;
    long long paintCount = 0;
    long long updateCount = 0;
    // Set when a paint is served without a new setFrame() since the last one.
    long long paintsWithoutNewImage = 0;

    // Whether the last frame was resampled to fit, and the size it was drawn
    // at: the scale factor is what decides whether filtering is good enough or
    // the downscale needs to move into the conversion.
    bool lastDrawWasScaled = false;
    bool lastDrawWasFiltered = false;
    QSize lastDrawSize;
};

// Hosts a VideoRenderer and owns the scheduling around it: when a repaint is
// asked for, how long it took to arrive, and which frame is current. What the
// pixels do on the way to the screen belongs to the renderer.
class ViewerWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);

    void setFrame(const trace::core::VideoFrame& frame);
    void clearImage();
    void setCenterText(const QString& text);
    const ViewerPerfStats& perfStats() const { return perfStats_; }
    const trace::core::VideoFrame& frame() const { return frame_; }
    QString rendererName() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;

private:
    std::unique_ptr<trace::render::VideoRenderer> renderer_;
    // Kept here as well as in the renderer so "which frame is displayed" is
    // answerable without asking the backend.
    trace::core::VideoFrame frame_;
    ViewerPerfStats perfStats_{};
    // Single monotonic source for this widget, so update()->paint latency is
    // measured against one clock rather than two independent timers.
    QElapsedTimer clock_;
    qint64 updateRequestedNs_ = -1;
};

} // namespace trace::ui
