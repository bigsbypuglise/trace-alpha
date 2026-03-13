#include "core/StillImageLoader.h"

#include <QFileInfo>

#ifdef TRACE_WITH_OIIO
#include <OpenImageIO/imageio.h>
#include <vector>
#include <cmath>
#endif

namespace trace::core {

bool StillImageLoader::load(const QString& path, LoadedImageInfo& out, QString& error) const {
    const QFileInfo fi(path);
    const QString ext = fi.suffix().toLower();

    if (ext == "exr") {
        return loadExr(path, out, error);
    }

    QImage img(path);
    if (img.isNull()) {
        error = QString("Failed to load image: %1").arg(path);
        return false;
    }

    out.filePath = path;
    out.fileName = fi.fileName();
    out.extension = ext;
    out.width = img.width();
    out.height = img.height();
    out.channels = 4;
    out.image = img;
    error.clear();
    return true;
}

bool StillImageLoader::loadExr(const QString& path, LoadedImageInfo& out, QString& error) const {
#ifdef TRACE_WITH_OIIO
    namespace oiio = OIIO;

    auto in = oiio::ImageInput::open(path.toStdString());
    if (!in) {
        error = QString("Failed to open EXR: %1").arg(path);
        return false;
    }

    const oiio::ImageSpec& spec = in->spec();
    const int width = spec.width;
    const int height = spec.height;
    const int channels = spec.nchannels;
    if (width <= 0 || height <= 0 || channels <= 0) {
        in->close();
        error = QString("Unsupported EXR dimensions/channels: %1").arg(path);
        return false;
    }

    std::vector<float> pixels(static_cast<size_t>(width) * static_cast<size_t>(height) * static_cast<size_t>(channels));
    if (!in->read_image(oiio::TypeDesc::FLOAT, pixels.data())) {
        in->close();
        error = QString("Failed to decode EXR pixels: %1").arg(path);
        return false;
    }
    in->close();

    QImage img(width, height, QImage::Format_RGBA8888);
    if (img.isNull()) {
        error = QString("Failed to allocate display image for EXR: %1").arg(path);
        return false;
    }

    auto toDisplay8 = [](float linear) -> int {
        if (linear < 0.0f) linear = 0.0f;
        // Simple linear -> display gamma mapping for now.
        float mapped = std::pow(linear, 1.0f / 2.2f);
        if (mapped > 1.0f) mapped = 1.0f;
        return static_cast<int>(mapped * 255.0f + 0.5f);
    };

    for (int y = 0; y < height; ++y) {
        auto* scanline = reinterpret_cast<QRgb*>(img.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const size_t idx = (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * static_cast<size_t>(channels);
            const float r = pixels[idx + 0];
            const float g = (channels > 1) ? pixels[idx + 1] : r;
            const float b = (channels > 2) ? pixels[idx + 2] : r;
            const float a = (channels > 3) ? pixels[idx + 3] : 1.0f;

            scanline[x] = qRgba(
                toDisplay8(r),
                toDisplay8(g),
                toDisplay8(b),
                toDisplay8(a)
            );
        }
    }

    out.filePath = path;
    out.fileName = QFileInfo(path).fileName();
    out.extension = "exr";
    out.width = width;
    out.height = height;
    out.channels = channels;
    out.image = img;
    error.clear();
    // TODO(trace-color): replace simple gamma mapping with OCIO display transform.
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(out);
    error = "EXR support not enabled (OpenImageIO not found at build time).";
    return false;
#endif
}

} // namespace trace::core
