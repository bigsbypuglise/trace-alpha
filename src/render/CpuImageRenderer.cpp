#include "render/CpuImageRenderer.h"

#include <QByteArray>
#include <QColor>
#include <QElapsedTimer>
#include <QPainter>
#include <QWidget>

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

void CpuImageRenderer::setPlaceholderText(const QString& text) {
    placeholder_ = text;
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
            const QSize fitted = image_.size().scaled(host->size(), Qt::KeepAspectRatio);
            QRect target((host->width() - fitted.width()) / 2,
                         (host->height() - fitted.height()) / 2,
                         fitted.width(), fitted.height());
            // Drawn in logical coordinates, but QPainter carries the widget's
            // device-pixel-ratio transform, so the rectangle actually sampled
            // into is `fitted * dpr`. That is the size the resample is really
            // against, and it is what D3D11 reports for the same rectangle --
            // the two disagreed by exactly the dpr until this was made explicit.
            const double dpr = host->devicePixelRatioF();
            const QSize drawn(qRound(fitted.width() * dpr), qRound(fitted.height() * dpr));
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
            const bool resampled = drawn != image_.size();
            const bool filtered = resampled && !nearestScaleForced();
            p.setRenderHint(QPainter::SmoothPixmapTransform, filtered);
            stats_.lastDrawWasScaled = resampled;
            stats_.lastDrawWasFiltered = filtered;
            stats_.lastDrawSize = drawn;
            QElapsedTimer drawTimer;
            drawTimer.start();
            p.drawImage(target, image_);
            drawImageMs = static_cast<double>(drawTimer.nsecsElapsed()) / 1'000'000.0;
        } else {
            p.setPen(QColor(150, 150, 150));
            p.drawText(host->rect(), Qt::AlignCenter, placeholder_);
        }

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

} // namespace trace::render
