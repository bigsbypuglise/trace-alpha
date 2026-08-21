#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <memory>

#include "core/VideoFrame.h"
#include "render/OverlayHooks.h"
#include "render/ViewScale.h"
#include "render/ViewTransform.h"

class QWidget;

namespace trace::render {

class OverlayModel;

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

    // What it cost to get one frame from system memory onto the GPU, and how
    // many GPU textures have ever been created. Zero on the CPU backend, which
    // has no upload and no textures -- and reading 0.00 there is the point: the
    // field says "this backend does not do this" rather than going missing.
    //
    // `textureCreates` is cumulative for the life of the renderer, deliberately.
    // Step 8 of the plan is "reuse textures and upload resources", and the only
    // way to know whether there is anything left to reuse is a count that does
    // not reset: a number that climbs during a run is churn, and a number that
    // stops climbing after the first frame of a source means the reuse is
    // already there. An average would hide both.
    double lastUploadMs = 0.0;
    double avgUploadMs = 0.0;
    long long uploadCount = 0;
    long long textureCreates = 0;

    // Taps per axis in the backend's downscale filter. 1 means a single bilinear
    // 2x2 tap, which undersamples at any meaningful reduction -- measured 0.74 of
    // the way from a correct area reduction to point sampling at 6.4x. Reported
    // because a filter that silently never engages looks identical to a working
    // one from every other number in this struct. 1 is the honest answer at 1:1
    // and for backends with no such filter.
    int reduceTaps = 1;
};

// How a frame becomes pixels on screen.
//
// The boundary is deliberately drawn so that frames stay renderer-agnostic:
// a VideoFrame is CPU-resident and refcounted, and a renderer treats its own
// GPU resources as presentation scratch uploaded from one. Switching backends,
// or losing a device, therefore costs re-uploading -- never a frame.
//
// The renderer owns the whole paint. Two painters cannot share one paint event,
// and a GPU backend will not be drawing through QPainter at all, so the host
// must not draw alongside it.
//
// The NO-FRAME state is no longer part of that. It was, and each backend
// carried its own copy of the hint string and its own way of putting it on
// screen -- a QPainter drawText here, a window-sized uploaded texture there.
// Since roadmap step 3 the empty state is a composited quad from OverlayModel,
// which both backends already draw, so it is one expression and one image
// instead of two that agree by inspection.
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

    // The floating transport this backend should composite into its own output,
    // or null for none. Non-owning: the host owns the model, because the model
    // outlives any one backend and because input has to reach it on the CPU
    // path through Qt events rather than through the renderer at all.
    //
    // BOTH backends implement this. It used to be `setOverlayHooks` and a
    // D3D11-only compositor, which was defensible while `cpu` was the default
    // and the GPU path was opt-in. `d3d11` is the default now and `cpu` is the
    // documented escape hatch, so an overlay only one of them can draw would
    // mean the advice "try TRACE_RENDERER=cpu" silently costs the user their
    // transport.
    virtual void setOverlay(OverlayModel* model) { (void)model; }

    // True when this backend initialized but cannot draw an overlay. Deliberately
    // separate from initialize() failing, because it was survivable while the
    // overlay was an off-by-default spike and stopped being survivable at spec
    // phase 6: the floating transport is now the ONLY transport, so a backend
    // that silently cannot draw it leaves the window with no controls at all.
    // The host reads this and falls back to the CPU backend, which always can.
    virtual bool overlayDrawFailed() const { return false; }

    // Hide or show the pointer over the video surface. Only a backend that owns
    // a native surface needs to act: it has its own window class with its own
    // cursor, so Qt's setCursor() on the host widget reaches nothing. The CPU
    // backend draws into the widget and inherits the host's cursor, which is why
    // the default here is to do nothing rather than to be pure virtual.
    virtual void setCursorHidden(bool hidden) { (void)hidden; }

    // How the frame is oriented on its way to the screen. A viewing transform
    // only: no decoded pixel, frame identity, cache entry or timing changes,
    // which is why it is here and not anywhere near the decoder.
    //
    // BOTH backends must honour it, from this one description. Implementing
    // rotate twice -- once in a shader, once in a QPainter -- is how two paths
    // end up differing by a flip, and the interface spec's own fallback
    // ("define the actions and defer the rendering") exists to prevent exactly
    // that. Identity is free on both: the CPU path skips installing a
    // QTransform, the GPU path uploads an identity matrix once and the vertex
    // shader multiplies by it.
    //
    // A backend that cannot honour it must not silently ignore it; there is no
    // such backend today, and adding one means answering this first.
    virtual void setViewTransform(const ViewTransform& transform) { (void)transform; }

    // The source's PIXEL aspect ratio -- width of one stored sample divided by
    // its height. 1.0 for everything with square pixels, which is every file in
    // the asset set; not 1.0 for anamorphic media, which Trace has drawn
    // squeezed for its whole existence because the fit was taken on stored
    // dimensions (spec section 4: "do not assume display ratio is always encoded
    // width divided by encoded height").
    //
    // It belongs beside setViewTransform for the same reason that one does:
    // it changes where the frame lands and nothing about the frame. Neither
    // backend needed a new code path for it -- the destination rect is
    // stretched, the sampling is in normalised coordinates, so every
    // subsampling, bit depth and the box-average reduction inherit it.
    //
    // CONTAINER ROTATION IS NOT HERE. It is composed into the ViewTransform by
    // the host, in one place, because it and the user's Rotate Left/Right are
    // the same operation applied for different reasons -- and two places
    // deciding "which way up" is exactly the divergence the transform contract
    // was built to prevent.
    virtual void setPixelAspect(double par) { (void)par; }

    // How large the picture is drawn and where it sits. Beside the two above
    // for the same reason both of those are here: it changes where the frame
    // lands and nothing about the frame.
    //
    // BOTH BACKENDS MUST HONOUR IT FROM THIS ONE DESCRIPTION, and they do it by
    // calling one shared expression rather than by each implementing the words
    // -- see viewDeviceRect(). A scale and an origin written twice is the
    // divergence ddb38ca had to fix for the fit that had no scale at all.
    virtual void setViewScale(const ViewScale& view) { (void)view; }

    // Whether the host WINDOW is fullscreen (owner decision 2026-08-21, the
    // fullscreen-aliasing item): the fullscreen FIT filters its magnification,
    // while deliberate zoom -- Actual Size, Zoom In, fullscreen or not -- keeps
    // phase 15's nearest. The renderer cannot ask the window itself (a child
    // HWND has no business walking its ancestors' Qt state), so the host pushes
    // the flag; ViewerWidget does it per paint, which is one bool compare and
    // needs no window-state event plumbing.
    virtual void setHostFullscreen(bool fullscreen) { (void)fullscreen; }

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

// The destination rect under a ViewScale: fitDeviceRect when fitting, and the
// reference scaled and panned otherwise. Spec phase 15's whole mechanism, and
// it is one function for the same reason the fit is -- a scale term and a pan
// origin implemented once in a shader and once in a QPainter is how two paths
// end up a pixel apart while every number agrees.
//
// `contentDisplayed` is the DELIVERED frame's on-screen size; the reference in
// the ViewScale is the full-resolution source's. They differ during a drag and
// the difference is the point -- see ViewScale::referenceDisplayed. The fit
// branch deliberately still fits the delivered frame, so it is unchanged
// arithmetic rather than the same answer by a new route.
QRect viewDeviceRect(QSize contentDisplayed, QSize deviceHost, const ViewScale& view);

// The pan offset a picture of this size is allowed inside a viewport of that
// size: zero on any axis where the picture already fits (so it stays centred
// and spec section 4's "no bars" holds), and otherwise bounded so an edge of
// the picture can never come inside the viewport. Shared because the host
// clamps the value it STORES and the renderer clamps the value it DRAWS, and
// the two agreeing by inspection is not the same as by construction.
QPointF clampPan(QSize picture, QSize deviceHost, QPointF pan);

// Whether the fullscreen fit filters its magnification (owner decision
// 2026-08-21, docs/fullscreen-aliasing-investigation.md option 2 narrowed to
// fullscreen: testers on panels larger than the source saw phase 15's nearest
// stair-step the fullscreen picture, and fullscreen is not deliberate zoom).
// TRACE_FS_MAG_FILTER=0 restores nearest there -- the rollback and the A/B
// control. Shared by both backends so one variable moves both, the
// TRACE_MAG_FILTER pattern.
bool fullscreenFitFilterEnabled();

// The largest scale this reference may be drawn at. IT IS A HARD BACKEND LIMIT
// EXPRESSED ONCE, not a taste judgement about how far a review tool should
// zoom: the D3D11 path letterboxes by VIEWPORT, and a D3D11_VIEWPORT is
// rejected outright above D3D11_VIEWPORT_BOUNDS_MAX -- which does not draw a
// distorted picture, it draws nothing at all, silently.
//
// So the ladder is built against this and viewDeviceRect clamps to it as well.
// The consequence is visible and worth stating rather than hiding: 4K media
// reaches 8:1, and a 9K plate stops at 2:1 because eight times its width is
// twice the bound.
double maxViewScale(QSize referenceDisplayed);

// Stored sample dimensions -> the size those samples are meant to be SHOWN at,
// before any view transform. Shared for the same reason the two above are: it
// is one line of arithmetic that both backends must do identically, and a copy
// in each is a copy that can drift.
//
// The larger axis is grown rather than the smaller shrunk, so the fit never
// starts from fewer pixels than the file has.
QSize applyPixelAspect(QSize pixelSize, double par);

// Builds the renderer selected by TRACE_RENDERER. **"d3d11" is the default** as
// of 2026-08-10, on any build that has it (Windows + MSVC + fxc); "cpu" is
// always available and is now the control and the escape hatch. An unknown or
// unavailable value warns and falls back to cpu, and name() then reports what
// was actually created.
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
