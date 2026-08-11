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
