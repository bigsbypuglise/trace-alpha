#include "render/VideoRenderer.h"

#include <QByteArray>
#include <QDebug>
#include <QWidget>

#include <algorithm>

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

    if (requested.isEmpty() || requested == "cpu") return createCpuRenderer();

#ifdef TRACE_WITH_D3D11
    if (requested == "d3d11") return std::make_unique<D3D11VideoRenderer>();
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
