#include "render/VideoRenderer.h"

#include <QByteArray>
#include <QDebug>

#include "render/CpuImageRenderer.h"

namespace trace::render {

std::unique_ptr<VideoRenderer> createRenderer() {
    const QByteArray requested = qgetenv("TRACE_RENDERER").toLower();

    if (!requested.isEmpty() && requested != "cpu") {
        // Say so rather than falling back silently. A GPU backend that never
        // engages while the app looks fine is the failure this message exists
        // to make impossible.
        qWarning().noquote()
            << "Trace: renderer" << QString::fromLatin1(requested)
            << "is not available in this build; using cpu.";
    }

    return std::make_unique<CpuImageRenderer>();
}

} // namespace trace::render
