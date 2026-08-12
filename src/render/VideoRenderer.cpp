#include "render/VideoRenderer.h"

#include <QByteArray>
#include <QDebug>
#include <QWidget>

#include <algorithm>
#include <cmath>

#include "render/CpuImageRenderer.h"
#ifdef TRACE_WITH_D3D11
#include "render/D3D11VideoRenderer.h"
#endif

namespace trace::render {

QSize hostDeviceSize(const QWidget* host) {
    if (!host) return QSize(1, 1);
    const double dpr = host->devicePixelRatioF();
    // Truncation, not rounding, and it is not arbitrary: it is what the
    // swapchain is sized to, so anything else would fit the picture to a
    // rectangle the back buffer does not have.
    //
    // qMax rather than std::max: windows.h arrives through the D3D11 backend's
    // header and defines a max() macro that breaks the qualified call.
    return QSize(qMax(1, static_cast<int>(host->width() * dpr)),
                 qMax(1, static_cast<int>(host->height() * dpr)));
}

QSize applyPixelAspect(QSize pixelSize, double par) {
    // Guard the degenerate values rather than trusting the caller: a par of 0
    // or a NaN would collapse the fit to nothing, and the value ultimately
    // comes from a file.
    if (pixelSize.isEmpty() || !(par > 0.0) || par == 1.0) return pixelSize;
    if (par > 1.0) {
        return QSize(qMax(1, static_cast<int>(std::lround(pixelSize.width() * par))),
                     pixelSize.height());
    }
    return QSize(pixelSize.width(),
                 qMax(1, static_cast<int>(std::lround(pixelSize.height() / par))));
}

QRect fitDeviceRect(QSize content, QSize deviceHost) {
    if (content.isEmpty()) return QRect(QPoint(0, 0), deviceHost);
    const QSize fitted = content.scaled(deviceHost, Qt::KeepAspectRatio);
    return QRect((deviceHost.width() - fitted.width()) / 2,
                 (deviceHost.height() - fitted.height()) / 2,
                 fitted.width(), fitted.height());
}

QPointF clampPan(QSize picture, QSize deviceHost, QPointF pan) {
    // Half the overhang on each axis. Negative when the picture is smaller than
    // the viewport, and the max with 0 turns that into "centred, no argument"
    // -- which is what makes a zoom back down to fit forget an offset the user
    // left behind at 4:1 instead of drifting the picture off centre.
    const double limitX = qMax(0.0, (picture.width() - deviceHost.width()) / 2.0);
    const double limitY = qMax(0.0, (picture.height() - deviceHost.height()) / 2.0);
    return QPointF(std::clamp(pan.x(), -limitX, limitX),
                   std::clamp(pan.y(), -limitY, limitY));
}

double maxViewScale(QSize referenceDisplayed) {
    // D3D11_VIEWPORT_BOUNDS_MAX. Named as a literal here rather than included
    // from d3d11.h because this file is built on every platform and the CPU
    // backend has to agree with the limit whether or not the GPU one exists --
    // a cap only one backend honoured would make TRACE_RENDERER=cpu zoom
    // further than the shipping path, which is the escape hatch behaving
    // differently in a way nobody would think to check.
    constexpr double kMaxExtent = 32768.0;
    const int longest = qMax(referenceDisplayed.width(), referenceDisplayed.height());
    if (longest <= 0) return 1.0;
    return qMax(1.0, kMaxExtent / static_cast<double>(longest));
}

QRect viewDeviceRect(QSize contentDisplayed, QSize deviceHost, const ViewScale& view) {
    // The fit path is the expression it always was, reached by one branch. A
    // "unified" version that fitted by computing a fit scale and running it
    // through the scale path would be the same answer by a different route,
    // and every recorded fit measurement in this project would be comparing
    // against arithmetic it was not taken on.
    if (view.fitToWindow || view.referenceDisplayed.isEmpty() || !(view.scale > 0.0)) {
        return fitDeviceRect(contentDisplayed, deviceHost);
    }

    const QSize ref = view.referenceDisplayed;
    // Clamped here as well as where the ladder is built. The ladder is a menu
    // and this is the draw, and only one of them is the last thing that runs.
    const double scale = qMin(view.scale, maxViewScale(ref));
    const int w = qMax(1, static_cast<int>(std::lround(ref.width() * scale)));
    const int h = qMax(1, static_cast<int>(std::lround(ref.height() * scale)));
    const QSize picture(w, h);
    const QPointF pan = clampPan(picture, deviceHost, view.pan);

    // Centred, then offset. Truncation matches fitDeviceRect's, so a scale of
    // exactly the fit ratio lands on the fit rect rather than a pixel beside it.
    return QRect(static_cast<int>((deviceHost.width() - w) / 2 + std::lround(pan.x())),
                 static_cast<int>((deviceHost.height() - h) / 2 + std::lround(pan.y())),
                 w, h);
}

std::unique_ptr<VideoRenderer> createCpuRenderer() {
    return std::make_unique<CpuImageRenderer>();
}

std::unique_ptr<VideoRenderer> createRenderer() {
    const QByteArray requested = qgetenv("TRACE_RENDERER").toLower();

    if (requested == "cpu") return createCpuRenderer();

#ifdef TRACE_WITH_D3D11
    // D3D11 IS THE DEFAULT as of GATE E's close (2026-08-10, owner decision).
    // Every gate that held it back has passed: GATE B with visual sign-off,
    // GATE C's planar upload, and GATE E's cadence work -- so the plan's
    // "TRACE_RENDERER=cpu stays the default until Gate E" has run its course.
    //
    // Measured on 4K ProRes 4444 against the cpu path: 0 doubled frames rather
    // than 1, 0 handlers over budget rather than 1, worst present gap 45.9ms
    // rather than 62.5, 99.8% of real time rather than 99.3%, and conversion
    // cost 5.6ms rather than 16.6.
    //
    // `TRACE_RENDERER=cpu` is now the control and the escape hatch, and it is
    // the first thing to try if anything about the picture looks wrong.
    if (requested.isEmpty() || requested == "d3d11") {
        return std::make_unique<D3D11VideoRenderer>();
    }
#else
    // Not a Windows/MSVC/fxc build. The default is the only backend there is,
    // and an explicit request for d3d11 falls through to the warning below --
    // which is the point: a build that cannot honour the request should say so
    // rather than quietly presenting through something else.
    if (requested.isEmpty()) return createCpuRenderer();
#endif

    // Say so rather than falling back silently. A GPU backend that never
    // engages while the app looks fine is the failure this message exists
    // to make impossible.
    qWarning().noquote()
        << "Trace: renderer" << QString::fromLatin1(requested)
        << "is not available in this build; using cpu.";
    return createCpuRenderer();
}

} // namespace trace::render
