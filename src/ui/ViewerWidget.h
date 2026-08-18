#pragma once

#include <QWidget>

#include <functional>
#include <QSize>
#include <QString>
#include <QElapsedTimer>
#include <memory>

#include "core/VideoFrame.h"
#include "render/OverlayModel.h"
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
    // Taps per axis in the backend's downscale filter; see RenderStats.
    int reduceTaps = 1;
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
    // Whether the floating transport is switched on at all. The HUD reports it,
    // because an overlay that silently never engages looks exactly like one
    // that is working from every other number -- the same reason rendererName()
    // is reported.
    bool overlayEnabled() const { return overlayModel_.enabled(); }
    // Read-only, for spec phase 14's accessibility proxy tree, which positions
    // itself from the model's own control rects rather than from a second
    // layout. Const on purpose: the proxies read geometry and never change it.
    const trace::render::OverlayModel& overlayModel() const { return overlayModel_; }
    // Reveal the floating transport and restart its idle timer. The host's entry
    // point for the one reveal source that is not a mouse event over the video:
    // "relevant keyboard input reveals it". Mouse reveals are handled inside the
    // model, on both backends' own input paths.
    void revealOverlay() { overlayModel_.reveal(); }
    // How much of the viewer's TOP EDGE the transient top chrome covers, in
    // logical pixels (UI redesign roadmap step 7). Held here in LOGICAL units
    // and converted at paint time, because the model wants device pixels and the
    // scale factor can change under a window that never moved.
    void setChromeTopInsetLogical(int px) { chromeTopInsetLogical_ = px; }
    // Hide or show the pointer over the video. Both paths are applied because
    // which one has effect depends on the adopted backend: the CPU renderer
    // draws into this widget and takes Qt's cursor, while the D3D11 surface is a
    // child HWND with its own class cursor that Qt cannot reach.
    void setCursorHidden(bool hidden);

    // UI redesign roadmap step 10, route 2 -- PROTOTYPE, off unless
    // TRACE_STRIP_BACKDROP=1.
    //
    // Where a tiny blurred copy of the top of each frame is delivered, so the
    // native top chrome can use it as a backdrop. It is computed HERE, at the
    // one place a frame arrives, rather than at any of MainWindow's several
    // paths that deliver one -- the same reason reclaimDecoder() is one choke
    // point rather than a convention observed at a dozen call sites.
    //
    // A std::function rather than a signal because it must run inline with the
    // frame: queued, it would deliver the PREVIOUS frame's backdrop, which is
    // the stale-instrument trap this project has recorded seven times.
    void setBackdropSink(std::function<void(const QImage&)> sink);

    // Recomputes and publishes the backdrop for the frame currently held, or
    // publishes null if there is none or the strip is not revealed.
    //
    // Called on every frame, and ALSO by MainWindow::syncTopChrome() on every
    // reveal, hide and media change -- because those are the moments the answer
    // changes without a frame arriving, and on a PAUSED file no frame ever
    // arrives. Gating on the reveal state without this would leave a revealed
    // strip blurring whatever was on screen when it last hid.
    void refreshBackdrop();

    // Rotate/flip. Held here as well as handed to the backend, so a renderer
    // that fails and is replaced by the CPU fallback inherits the orientation
    // rather than quietly resetting it -- the same reason the overlay model
    // lives here.
    void setViewTransform(const trace::render::ViewTransform& transform);
    // The USER's transform -- what the Edit menu set and what Reset returns to.
    // Not what the renderer is drawing with, which is this composed with the
    // container's own rotation. The menu, the HUD and Reset all want this one:
    // a file that carries rot90 must not make Reset look like it failed.
    const trace::render::ViewTransform& viewTransform() const { return viewTransform_; }

    // The shape the MEDIA states it should be shown as: its pixel aspect ratio
    // and the clockwise rotation its container asks for (spec phase 12). Session
    // state and media state are kept apart here and composed in one place --
    // see applySourceShape().
    void setSourceShape(double pixelAspect, int rotationDegrees);
    double sourcePixelAspect() const { return sourcePixelAspect_; }
    int sourceRotationDegrees() const { return sourceRotationDegrees_; }
    // The on-screen display aspect: the media's shape with the user's transform
    // composed onto it. This is what the window is sized to, and it is here
    // rather than in MainWindow because the composition rule lives here.
    double displayedAspect(QSize sourcePixels) const;
    // The shape the minimum size is computed for. 16:9 until media says
    // otherwise, which reproduces the old fixed 640x360 exactly.
    void setMinimumAspect(double aspect);

    // ---- View scaling (spec phase 15) ---------------------------------------
    //
    // The state lives here, beside the transform and for the same reasons: a
    // backend that fails and is replaced by the CPU fallback must inherit the
    // zoom rather than quietly returning to fit, and the composition with the
    // media's own shape has to happen in ONE place or the two backends can
    // disagree about how big the picture is.

    // The full-resolution STORED size of the source. Separate from the frame
    // the renderer is holding, which during a drag is a viewport-sized preview
    // -- see ViewScale::referenceDisplayed for why scaling that instead would
    // make the picture jump on release.
    void setSourcePixelSize(QSize pixels);

    void setFitToWindow();
    void setActualSize();
    // +1 in, -1 out. One rung of the ladder, from wherever the picture is now
    // -- including from fit, whose scale is an arbitrary ratio rather than a
    // rung, which is why the step is "the next rung past here" and not
    // "multiply by two".
    void zoomStep(int direction);
    // True while the picture is larger than the viewport on either axis, which
    // is the only state in which a pan can move anything.
    bool canPan() const;
    void panBy(QPointF deltaDevice);
    // Device pixels per displayed source pixel, INCLUDING while fitting. The
    // fit's value is computed from the current geometry rather than read back
    // from the last paint: `lastDrawSize` is measured BY the paint, so a menu
    // built from it would report the size before the resize that opened it.
    double currentScale() const;
    bool isFitToWindow() const { return viewScale_.fitToWindow; }
    // The full-resolution source's on-screen size, with the pixel aspect and
    // the composed transform applied. What the scale scales.
    QSize displayedSourceSize() const;

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    QPaintEngine* paintEngine() const override;
    // The CPU backend's input path. Under the D3D11 backend these never fire:
    // the surface is a child HWND above this widget and takes the hit-test
    // itself, which is the same fact that closed the Qt-widget overlay route.
    // So the two paths do not race -- exactly one of them is reachable at a
    // time, decided by which backend was adopted.
    void mouseMoveEvent(QMouseEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    // Applies the widget-level contract a backend asks for, then initializes
    // it. False leaves the widget in its non-native state, ready for a fallback.
    bool adoptRenderer(std::unique_ptr<trace::render::VideoRenderer> renderer, QString& error);
    // Logical widget coordinates -> the surface device pixels the model lays
    // itself out in. One conversion, in one place, because the D3D11 path has
    // no conversion at all and a second expression is how the two would drift.
    QPoint toDevice(const QPointF& logical) const;

    // Declared BEFORE renderer_: the constructor adopts a renderer, and
    // adoptRenderer hands it &overlayModel_. A member constructed afterwards
    // would be handed to the backend before it existed.
    trace::render::OverlayModel overlayModel_;
    trace::render::ViewTransform viewTransform_{};
    double sourcePixelAspect_ = 1.0;
    int sourceRotationDegrees_ = 0;
    // Pushes the composition of viewTransform_ and the source's own rotation at
    // the renderer, plus the pixel aspect. The one place either reaches a
    // backend. It also re-pushes the view scale, because the reference the
    // scale is measured against is that same composition -- a quarter turn
    // exchanges the axes of the thing being scaled.
    void applySourceShape();
    // The one place the view scale reaches a backend, and the one place the
    // reference size is computed.
    void applyViewScale();
    // The scale a fit-to-window draw is currently using. Computed from the
    // geometry, never read back from RenderStats.
    double fitScale() const;
    // Set the scale, keeping the viewport's centre on the same point of the
    // picture. Every route to a non-fit scale goes through it, so the anchoring
    // is a property of the state rather than of each caller.
    void setScaleAnchored(double scale);
    QSize pictureDeviceSize() const;
    trace::render::ViewScale viewScale_{};
    QSize sourcePixels_;
    int chromeTopInsetLogical_ = 0;
    // Step 10 route 2 prototype; null unless TRACE_STRIP_BACKDROP=1.
    std::function<void(const QImage&)> backdropSink_;
    double minimumAspect_ = 16.0 / 9.0;
    // The settled floating-transport width (spec phase 6). Named here rather
    // than reached for from OverlayModel because this is a FLOOR expressed in
    // the same units, not a second definition of the panel: if the panel width
    // is ever reopened as an owner decision, both move together and a grep for
    // 460 finds them.
    static constexpr int kMinTransportWidth = 460;
    void applyMinimumForAspect();
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
