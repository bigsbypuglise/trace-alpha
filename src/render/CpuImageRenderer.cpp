#include "render/CpuImageRenderer.h"

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QPainter>
#include <QRectF>
#include <QRgb>
#include <QWidget>

#include <algorithm>
#include <cmath>

namespace trace::render {
namespace {

// Filtering the fit-to-window resample costs real time per paint at 4K.
// TRACE_NEAREST_SCALE=1 restores the old point-sampled blit so the two can be
// A/B'd against the HUD's `draw` figure without a rebuild.
bool nearestScaleForced() {
    static const bool on = [] {
        const QByteArray v = qgetenv("TRACE_NEAREST_SCALE");
        return !v.isEmpty() && v != "0";
    }();
    return on;
}

// ABOVE 1:1 THE PICTURE IS PIXELS, NOT A SMOOTHED FRAME -- owner decision,
// 2026-08-11, spec phase 15. Someone who has zoomed a review tool to 4:1 is
// inspecting the samples, and a bilinear magnification shows them a blend of
// samples that is not in the file. TRACE_MAG_FILTER=linear restores the
// smoothed magnification for the A/B, on both backends, so the decision can be
// looked at rather than argued about.
//
// Reduction is untouched and stays filtered: undersampling a downscale is step
// 9's defect and it is signed off.
bool magnifyLinearForced() {
    static const bool on = [] {
        const QByteArray v = qgetenv("TRACE_MAG_FILTER").toLower();
        return v == "linear" || v == "bilinear" || v == "1";
    }();
    return on;
}

} // namespace

bool CpuImageRenderer::initialize(QWidget* host, QString& error) {
    Q_UNUSED(host);
    Q_UNUSED(error);
    // Nothing to acquire: QPainter needs no device, which is the reason this
    // backend can never fail to initialize and is therefore the fallback.
    return true;
}

void CpuImageRenderer::setFrame(const trace::core::VideoFrame& frame) {
    frame_ = frame;
    image_ = frame_.toQImage();
    hasImage_ = !image_.isNull();
}

void CpuImageRenderer::clearFrame() {
    frame_ = trace::core::VideoFrame{};
    image_ = QImage();
    hasImage_ = false;
}

void CpuImageRenderer::resize(QSize size) {
    // The blit refits from the widget rect on every paint, so there is no
    // renderer state to invalidate here.
    Q_UNUSED(size);
}

void CpuImageRenderer::paint(QWidget* host) {
    if (!host) return;

    QElapsedTimer timer;
    timer.start();
    double drawImageMs = 0.0;

    // Inner scope so ~QPainter (which flushes to the backing store) runs
    // before the total is taken; the original timer stopped short of it.
    {
        QPainter p(host);
        p.fillRect(host->rect(), QColor(0, 0, 0));

        if (hasImage_) {
            // Fit on the DEVICE grid -- the grid the frame is actually
            // rasterised onto -- using the shared arithmetic, then express the
            // result back in the logical coordinates QPainter takes. Fitting in
            // logical pixels instead and letting the dpr transform land the rect
            // where it may is what put this backend a fraction of a pixel away
            // from D3D11 at fractional ratios; see hostDeviceSize().
            const double dpr = host->devicePixelRatioF();
            // Fit the DISPLAYED size, exactly as the D3D11 backend does: a
            // quarter turn exchanges the axes, so a 16:9 source letterboxes as
            // 9:16 and the destination rect has to follow.
            // Pixel aspect first, then the transform -- see the identical line
            // in the D3D11 backend for why that order, and applyPixelAspect for
            // why the arithmetic is shared rather than written twice.
            const QSize displayed = transform_.apply(applyPixelAspect(image_.size(), pixelAspect_));
            // Spec phase 15: the fit is one case of this, taken by one branch
            // inside the shared expression. What comes back can be LARGER than
            // the host and can start outside it, which is what Actual Size on
            // 4K media in a capped window means; QPainter clips the blit to the
            // widget, so the cost tracks the visible area rather than the
            // picture's.
            const QRect dev = viewDeviceRect(displayed, hostDeviceSize(host), viewScale_);
            const QSize drawn = dev.size();
            const QRectF target(dev.x() / dpr, dev.y() / dpr,
                                dev.width() / dpr, dev.height() / dpr);
            // Nearest-neighbour point sampling is what jagged every diagonal
            // edge in the frame: any window that is not exactly the source
            // resolution drops whole pixel rows and columns. Filter whenever
            // the frame is being resampled -- but never when it maps 1:1, where
            // filtering could only soften pixels the user is inspecting.
            //
            // The test is against the device size for the same reason: at
            // dpr 1.5 a frame fitted to a logical rect of its own size is being
            // upscaled by half again, and comparing logical sizes called that
            // 1:1 and switched filtering off for it.
            const bool resampled = drawn != displayed;
            // ONE PREDICATE, AND IT SUBSUMES THE 1:1 SPECIAL CASE RATHER THAN
            // SITTING BESIDE IT. The old rule was "filter whenever resampled",
            // with 1:1 falling out because an unresampled frame is not
            // resampled. The rule is now "filter only when REDUCING", and 1:1
            // still falls out of it -- so the exactly-1:1 behaviour this
            // backend has had since the filter was added is unchanged, and
            // magnification joins it instead of being a second exception.
            const bool reducing = drawn.width() < displayed.width()
                                  || drawn.height() < displayed.height();
            // The fullscreen FIT filters its magnification (owner,
            // 2026-08-21) -- same predicate as the D3D11 backend's, and the
            // deliberate-zoom case keeps nearest because fitToWindow is false
            // there. TRACE_FS_MAG_FILTER=0 is the rollback.
            const bool fsFitFilter = hostFullscreen_ && viewScale_.fitToWindow
                                     && fullscreenFitFilterEnabled();
            const bool filtered = resampled && !nearestScaleForced()
                                  && (reducing || magnifyLinearForced() || fsFitFilter);
            p.setRenderHint(QPainter::SmoothPixmapTransform, filtered);
            stats_.lastDrawWasScaled = resampled;
            stats_.lastDrawWasFiltered = filtered;
            stats_.lastDrawSize = drawn;
            QElapsedTimer drawTimer;
            drawTimer.start();
            if (transform_.isIdentity()) {
                p.drawImage(target, image_);
            } else {
                // Rotate and flip about the CENTRE of the destination rect, then
                // draw the image into the rect it would have occupied before the
                // axes were exchanged. Doing it this way rather than pre-rotating
                // the QImage matters: QImage::transformed() would allocate and
                // copy a whole frame every paint, which at 4K is ~37MB of work
                // per present for a viewing change that costs the GPU one 2x2
                // multiply.
                //
                // The flip is applied AFTER the rotation, in the destination's
                // own frame, so "flip horizontally" flips what the user can see
                // -- the same composition order the shader's matrix uses.
                p.save();
                p.translate(target.center());
                // scale BEFORE rotate in call order, which means the rotation is
                // applied to the geometry FIRST and the flip second -- QPainter
                // post-multiplies, so the last transform named is the innermost.
                // Naming them the other way round is the same two operations in
                // the opposite order, and for a quarter turn plus a horizontal
                // flip that is a VERTICAL flip on screen: the two backends would
                // then differ by a mirror while every number agreed.
                p.scale(transform_.flipH ? -1.0 : 1.0, transform_.flipV ? -1.0 : 1.0);
                p.rotate(transform_.quarterTurns * 90.0);
                const QSizeF pre = transform_.swapsAxes()
                    ? QSizeF(target.height(), target.width())
                    : target.size();
                p.drawImage(QRectF(QPointF(-pre.width() / 2.0, -pre.height() / 2.0), pre),
                            image_);
                p.restore();
            }
            drawImageMs = static_cast<double>(drawTimer.nsecsElapsed()) / 1'000'000.0;
        }
        // No `else` any more. With no frame the stage is the fill above, and
        // the empty state is drawn by the overlay pass below -- one expression
        // shared with the D3D11 backend rather than a QPainter placeholder
        // here and a full-window uploaded texture there.

        // After the video, like the D3D11 backend, and inside the same painter
        // scope so it is included in the paint total rather than measured
        // separately into a number nobody compares.
        if (overlayModel_) paintOverlay(p, host);

        // Same scope as the original measurement, for continuity.
        const double paintMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
        stats_.lastPaintMs = paintMs;
        ++stats_.samples;
        const double n = static_cast<double>(stats_.samples);
        stats_.avgPaintMs += (paintMs - stats_.avgPaintMs) / n;
    }

    const double paintTotalMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    stats_.lastPaintTotalMs = paintTotalMs;
    stats_.lastDrawImageMs = drawImageMs;
    ++stats_.paintCount;
    const double pn = static_cast<double>(stats_.paintCount);
    stats_.avgPaintTotalMs += (paintTotalMs - stats_.avgPaintTotalMs) / pn;
    stats_.avgDrawImageMs += (drawImageMs - stats_.avgDrawImageMs) / pn;
}

const QImage& CpuImageRenderer::tintedAtlas(const QImage& atlas, long long revision,
                                            float brighten) {
    for (auto& t : tints_) {
        if (std::abs(t.brighten - brighten) < 1e-4f) {
            if (t.revision != revision) {
                t.image = atlas;
                // Detach before writing: `atlas` is the model's own image and
                // must not be modified through a shared buffer.
                t.image.detach();
                const int h = t.image.height();
                const int w = t.image.width();
                for (int y = 0; y < h; ++y) {
                    auto* line = reinterpret_cast<QRgb*>(t.image.scanLine(y));
                    for (int x = 0; x < w; ++x) {
                        const QRgb c = line[x];
                        const int a = qAlpha(c);
                        if (a == 0) continue;
                        // Premultiplied RGB scaled and alpha left alone, which
                        // is bit-for-bit what the shader's tint does. Values
                        // above the alpha are deliberately allowed: both
                        // compositors evaluate src + dst*(1-srcA), so an
                        // over-bright premultiplied texel is an additive glow
                        // on each and clamping here would make the CPU path
                        // the odd one out.
                        line[x] = qRgba(std::min(255, static_cast<int>(qRed(c) * brighten)),
                                        std::min(255, static_cast<int>(qGreen(c) * brighten)),
                                        std::min(255, static_cast<int>(qBlue(c) * brighten)),
                                        a);
                    }
                }
                t.revision = revision;
            }
            return t.image;
        }
    }
    tints_.push_back(TintCache{brighten, -1, QImage()});
    return tintedAtlas(atlas, revision, brighten);
}

// The CPU half of the renderer-neutral overlay contract. It consumes exactly
// the same quads the D3D11 drawer does, in the same order, in the same device-
// pixel space -- so the two paths differ in how a rectangle reaches the screen
// and in nothing else.
//
// QPainter is given the DEVICE grid explicitly rather than being left on the
// widget's logical one. The model lays out in device pixels because that is the
// only space the D3D11 surface has, and re-expressing every rect in logical
// coordinates here would put the two backends a fraction of a pixel apart at
// fractional DPI, which is the exact fault ddb38ca was opened to fix for the
// video rect.
void CpuImageRenderer::paintOverlay(QPainter& p, QWidget* host) {
    const QSize pixels = hostDeviceSize(host);
    overlayModel_->setDevicePixelRatio(host->devicePixelRatioF());
    // The same member the draw branch above reads, so "is there a picture" and
    // "is the empty state showing" cannot come to disagree.
    overlayModel_->setMediaPresent(hasImage_);
    const auto& quads = overlayModel_->buildFrame(pixels);
    if (quads.empty()) return;

    const QImage& atlas = overlayModel_->atlasImage();
    const QImage& text = overlayModel_->textImage();
    const QImage& message = overlayModel_->messageImage();
    const QImage& empty = overlayModel_->emptyImage();
    const QImage& readout = overlayModel_->readoutImage();
    // No blanket atlas guard: a message quad can be the only thing to show,
    // before the panel has ever been revealed and the atlas rasterised. Each
    // quad checks its own source below -- the same rule the D3D11 drawer
    // follows, so the two backends decline the same quads.

    p.save();
    // Undo the dpr transform Qt installed, so a quad's device-pixel rect means
    // device pixels here too.
    p.resetTransform();
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setCompositionMode(QPainter::CompositionMode_SourceOver);

    for (const auto& q : quads) {
        const QImage* src = nullptr;
        switch (q.source) {
            case OverlayQuad::Source::Text:
                if (!text.isNull()) src = &text;
                break;
            case OverlayQuad::Source::Message:
                if (!message.isNull()) src = &message;
                break;
            case OverlayQuad::Source::Empty:
                if (!empty.isNull()) src = &empty;
                break;
            // UI redesign roadmap step 5: the two time readouts either side of
            // the track, two halves of one image.
            case OverlayQuad::Source::Readout:
                if (!readout.isNull()) src = &readout;
                break;
            case OverlayQuad::Source::Atlas:
                if (!atlas.isNull())
                    src = &tintedAtlas(atlas, overlayModel_->atlasRevision(), q.brighten);
                break;
        }
        if (!src) continue;
        p.setOpacity(q.alpha);
        p.drawImage(q.dst, *src, q.src);
    }
    p.setOpacity(1.0);
    p.restore();
}

} // namespace trace::render
