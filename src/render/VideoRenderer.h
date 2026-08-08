#pragma once

#include <QRect>
#include <QSize>
#include <QString>
#include <memory>

#include "core/VideoFrame.h"

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

    // Identifies the backend in the HUD, so a fallback is visible rather than
    // silent -- a GPU path that quietly never engages is the failure mode worth
    // designing against.
    virtual QString name() const = 0;
    virtual const RenderStats& stats() const = 0;
};

// Builds the renderer selected by TRACE_RENDERER. Only "cpu" exists today and
// it is the default; an unknown or unavailable value falls back to it, and
// name() then reports what was actually created.
std::unique_ptr<VideoRenderer> createRenderer();

} // namespace trace::render
