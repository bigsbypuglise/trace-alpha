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
    // at, in DEVICE pixels -- mirrored from RenderStats, which is where the
    // unit is stated and why.
    bool lastDrawWasScaled = false;
    bool lastDrawWasFiltered = false;
    QSize lastDrawSize;

    // CPU -> GPU frame upload cost and cumulative texture creations, mirrored
    // from RenderStats where the units and the reason for a cumulative count are
    // stated. Both read 0 on the CPU backend, which has neither.
    double lastUploadMs = 0.0;
    double avgUploadMs = 0.0;
    long long uploadCount = 0;
    long long textureCreates = 0;
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
    // Whether the adopted backend can take Y/U/V planes and convert them
    // itself, so the decoder may skip swscale for full-resolution frames.
    bool rendererAcceptsPlanarYuv() const;
    // True when the backend TRACE_RENDERER selected failed to initialize and the
    // CPU backend was adopted in its place. rendererName() alone cannot answer
    // this: the D3D11 backend renames itself "d3d11 (warp)" when it lands on the
    // software rasteriser, so comparing names before and after would read a
    // successful WARP init as a fallback and a genuine fallback as a rename.
    bool rendererFellBack() const { return rendererFellBack_; }
    void setOverlayHooks(const trace::render::OverlayHooks& hooks);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    QPaintEngine* paintEngine() const override;

private:
    // Applies the widget-level contract a backend asks for, then initializes
    // it. False leaves the widget in its non-native state, ready for a fallback.
    bool adoptRenderer(std::unique_ptr<trace::render::VideoRenderer> renderer, QString& error);

    std::unique_ptr<trace::render::VideoRenderer> renderer_;
    // Mirrors the active renderer's usesNativeSurface(), so the widget-level
    // attributes can be undone if a backend fails to initialize and the CPU
    // fallback is adopted in its place.
    bool nativeSurface_ = false;
    bool hostHwndSpike_ = false;
    bool rendererFellBack_ = false;
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
