#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <memory>

#include "core/VideoFrame.h"
#include "render/OverlayHooks.h"

class QWidget;

namespace trace::render {

// What a render pass cost and what it produced. Kept separate from
// ViewerPerfStats because these are the renderer's numbers: a D3D11 backend
// reports the same fields for a completely different set of operations, and the
// HUD should not have to know which backend produced them.
struct RenderStats {
    // Scope note carried over from ViewerWidget: lastPaintMs covers the paint
    // body only, stopping before the painter flushes. lastPaintTotalMs includes
    // the flush. Neither alone is total presentation cost.
    double lastPaintMs = 0.0;
    double avgPaintMs = 0.0;
    long long samples = 0;

    double lastDrawImageMs = 0.0;   // the blit itself, without the surrounding setup
    double avgDrawImageMs = 0.0;
    double lastPaintTotalMs = 0.0;
    double avgPaintTotalMs = 0.0;
    long long paintCount = 0;

    // Whether the last frame was resampled to fit, and the size it was drawn
    // at: the scale factor is what decides whether filtering is good enough or
    // the downscale needs to move into the conversion.
    //
    // lastDrawSize is in DEVICE pixels, and `lastDrawWasScaled` is decided
    // against it. Backends must agree on this: a logical-pixel answer differs
    // from a device-pixel one by the dpr for the same on-screen rectangle, and
    // the two backends reported different numbers for the same picture until
    // the unit was named here.
    bool lastDrawWasScaled = false;
    bool lastDrawWasFiltered = false;
    QSize lastDrawSize;
};

// How a frame becomes pixels on screen.
//
// The boundary is deliberately drawn so that frames stay renderer-agnostic:
// a VideoFrame is CPU-resident and refcounted, and a renderer treats its own
// GPU resources as presentation scratch uploaded from one. Switching backends,
// or losing a device, therefore costs re-uploading -- never a frame.
//
// The renderer owns the whole paint, including the no-frame placeholder. Two
// painters cannot share one paint event, and a GPU backend will not be drawing
// through QPainter at all, so the host must not draw alongside it.
class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;

    // `host` is the widget being drawn into. A GPU backend takes its native
    // handle here; the CPU backend only needs it at paint time.
    virtual bool initialize(QWidget* host, QString& error) = 0;

    // The frame to display from now on. Cheap for the CPU backend (a refcount
    // bump and a view); an upload for a GPU one.
    virtual void setFrame(const trace::core::VideoFrame& frame) = 0;
    virtual void clearFrame() = 0;
    // Shown when there is no frame.
    virtual void setPlaceholderText(const QString& text) = 0;

    virtual void resize(QSize size) = 0;
    virtual void paint(QWidget* host) = 0;

    // True when the backend can present separate Y/U/V planes and apply the
    // colour matrix itself, so the decoder may skip swscale for full-resolution
    // frames (GATE C). False means every frame must arrive as BGRA8.
    //
    // Asked of the renderer rather than inferred from name() for the same
    // reason as usesNativeSurface(): it is a capability the application has to
    // know before it configures the decoder, and a second backend with the same
    // answer should not have to be recognised by name.
    virtual bool acceptsPlanarYuv() const { return false; }

    // True when the backend presents through a native surface rather than
    // Qt's backing store. The host must then realise a native window for it to
    // attach to and stop erasing the widget -- neither of which the renderer
    // can do for itself, because they are properties of the widget and have to
    // be set before initialize() runs.
    //
    // This is a widget-level contract, not a rendering detail, which is why it
    // is asked here rather than inferred from name().
    virtual bool usesNativeSurface() const { return false; }

    // Wires the application's commands into a backend that draws its own
    // controls. A no-op for backends that do not: the CPU path hosts ordinary
    // Qt widgets, which need none of this.
    virtual void setOverlayHooks(const OverlayHooks& hooks) { (void)hooks; }

    // Identifies the backend in the HUD, so a fallback is visible rather than
    // silent -- a GPU path that quietly never engages is the failure mode worth
    // designing against.
    virtual QString name() const = 0;
    virtual const RenderStats& stats() const = 0;
};

// The host widget's size in device pixels, and the fitted destination rect
// within it -- the two pieces of arithmetic every backend needs and both must
// do identically.
//
// They are here rather than in each backend because they were written twice and
// the copies disagreed. The CPU path fitted in logical pixels and let QPainter's
// dpr transform land the rect where it may, while D3D11 fitted in device pixels;
// at a fractional ratio that put the two destination rectangles a fraction of a
// pixel apart, and the sampling phase offset that follows made the same frame
// differ on 5-13% of the video band at dpr 1.25 and 1.5 while matching exactly
// at dpr 1 and dpr 2. Even the truncation matters: qRound and static_cast<int>
// of the same device size differ by one pixel often enough to move the centred
// rect by half of one.
QSize hostDeviceSize(const QWidget* host);
QRect fitDeviceRect(QSize content, QSize deviceHost);

// Builds the renderer selected by TRACE_RENDERER. "cpu" is the default and
// always available; "d3d11" is opt-in and only exists in a Windows build. An
// unknown or unavailable value warns and falls back to cpu, and name() then
// reports what was actually created.
//
// This can only decline a backend it knows cannot run. A backend that fails in
// initialize() is the host's problem to fall back from, because only the host
// has the widget -- see ViewerWidget.
std::unique_ptr<VideoRenderer> createRenderer();

// The CPU backend, unconditionally. The fallback path needs to build one
// directly after a GPU backend has failed to initialize, without going back
// through TRACE_RENDERER and being handed the same failing backend again.
std::unique_ptr<VideoRenderer> createCpuRenderer();

} // namespace trace::render
