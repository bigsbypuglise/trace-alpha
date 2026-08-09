#include "render/VideoRenderer.h"

#include <QByteArray>
#include <QDebug>

#include "render/CpuImageRenderer.h"
#ifdef TRACE_WITH_D3D11
#include "render/D3D11VideoRenderer.h"
#endif

namespace trace::render {

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
