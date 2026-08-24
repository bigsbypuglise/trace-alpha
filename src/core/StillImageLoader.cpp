#include "core/StillImageLoader.h"

#include <QFileInfo>

#ifdef TRACE_WITH_OIIO
#include <OpenImageIO/imageio.h>
#include <algorithm>
#include <cstring>
#include <vector>
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

    out = LoadedImageInfo{};
    out.filePath = path;
    out.fileName = fi.fileName();
    out.extension = ext;
    out.width = img.width();
    out.height = img.height();
    // QImage does not report the file's channel count and every format it reads
    // here is display-referred RGB(A), so 4 is the buffer AND the honest reading
    // of the source. On the EXR path the two differ, which is why the field
    // means the source everywhere else.
    out.channels = img.hasAlphaChannel() ? 4 : 3;
    out.buffer = FrameBuffer::adopt(std::move(img));
    if (!out.buffer) {
        error = QString("Unsupported image format: %1").arg(path);
        return false;
    }
    error.clear();
    return true;
}

#ifdef TRACE_WITH_OIIO
namespace {

// Which pass to show, given what the user asked for and what the file has.
//
// Falling back rather than failing is deliberate: a sequence whose frames do
// not all carry the same AOVs must keep playing, showing the nearest thing it
// has, rather than stopping on the first frame that is missing a pass.
int choosePass(const std::vector<ExrPass>& passes, const QString& preferred) {
    if (passes.empty()) return -1;

    if (!preferred.isEmpty()) {
        for (std::size_t i = 0; i < passes.size(); ++i)
            if (passes[i].layer.compare(preferred, Qt::CaseInsensitive) == 0)
                return static_cast<int>(i);
    }

    // The root group is the composite and is what the file is "of".
    for (std::size_t i = 0; i < passes.size(); ++i)
        if (passes[i].layer.isEmpty() && passes[i].cls == PassClass::Colour)
            return static_cast<int>(i);

    for (std::size_t i = 0; i < passes.size(); ++i)
        if (passes[i].cls == PassClass::Colour) return static_cast<int>(i);

    return 0;
}

} // namespace
#endif

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
    const int nchannels = spec.nchannels;
    if (width <= 0 || height <= 0 || nchannels <= 0) {
        in->close();
        error = QString("Unsupported EXR dimensions/channels: %1").arg(path);
        return false;
    }

    QStringList names;
    names.reserve(nchannels);
    for (int c = 0; c < nchannels; ++c)
        names << QString::fromStdString(spec.channelnames[static_cast<std::size_t>(c)]);

    const std::vector<ExrPass> passes = groupExrChannels(names);
    const int active = choosePass(passes, preferredPass_);
    if (active < 0) {
        in->close();
        error = QString("No displayable channels in EXR: %1").arg(path);
        return false;
    }
    const ExrPass& pass = passes[static_cast<std::size_t>(active)];

    // RGBA FLOAT, SCENE-REFERRED, NOT CLAMPED.
    //
    // Four components rather than three so one layout serves every pass and the
    // colour stage has one descriptor to write against. The 25% that costs
    // against an RGB-only layout buys a single code path through the transform,
    // the display mapping and the frame cache.
    auto buffer = FrameBuffer::allocate(width, height, PixelLayout::RGBAF32);
    if (!buffer) {
        in->close();
        error = QString("Out of memory allocating EXR frame: %1").arg(path);
        return false;
    }
    uint8_t* base = buffer->data();
    const int stride = buffer->bytesPerLine();

    const int begin = pass.spanBegin();
    const int end = pass.spanEnd();
    const bool hasAlpha = pass.hasAlpha();

    // ONLY THE CHANNELS BEING DISPLAYED ARE READ. The previous version asked for
    // spec.nchannels, which on the 27-channel Redshift file allocated and decoded
    // 224 MB per frame to use 24 MB of it -- correct, and the wrong shape for a
    // 97-frame sequence.
    //
    // FAST PATH: when the pass's components are already contiguous and in R,G,B
    // (,A) order -- which is what OpenImageIO presents for every pass in the
    // test set, measured rather than assumed -- OIIO can scatter straight into
    // the RGBA float buffer through the pixel stride, with no intermediate
    // allocation at all.
    const bool contiguous =
        pass.channel[0] == begin && pass.channel[1] == begin + 1 &&
        pass.channel[2] == begin + 2 &&
        (!hasAlpha || pass.channel[3] == begin + 3);

    bool readOk = false;
    if (contiguous) {
        if (!hasAlpha) {
            // Pre-fill alpha. The read below advances by a whole 16-byte pixel
            // and writes three floats, so it never touches this.
            for (int y = 0; y < height; ++y) {
                float* row = reinterpret_cast<float*>(base + static_cast<std::size_t>(y) * stride);
                for (int x = 0; x < width; ++x) row[x * 4 + 3] = 1.0f;
            }
        }
        readOk = in->read_image(0, 0, begin, hasAlpha ? begin + 4 : begin + 3,
                                oiio::TypeDesc::FLOAT, base,
                                static_cast<oiio::stride_t>(4 * sizeof(float)),
                                static_cast<oiio::stride_t>(stride));
    } else {
        // The general case: read the pass's span and scatter by NAME. Reached by
        // a single-channel data pass replicated across RGB, and by any file
        // whose channels are ordered differently from the ones measured here.
        const int span = end - begin;
        std::vector<float> tmp(static_cast<std::size_t>(width) *
                               static_cast<std::size_t>(height) *
                               static_cast<std::size_t>(span));
        readOk = in->read_image(0, 0, begin, end, oiio::TypeDesc::FLOAT, tmp.data());
        if (readOk) {
            const int ci[4] = {pass.channel[0] - begin, pass.channel[1] - begin,
                               pass.channel[2] - begin,
                               hasAlpha ? pass.channel[3] - begin : -1};
            for (int y = 0; y < height; ++y) {
                float* row = reinterpret_cast<float*>(base + static_cast<std::size_t>(y) * stride);
                const float* src = tmp.data() + static_cast<std::size_t>(y) *
                                                    static_cast<std::size_t>(width) *
                                                    static_cast<std::size_t>(span);
                for (int x = 0; x < width; ++x) {
                    const float* p = src + static_cast<std::size_t>(x) * span;
                    for (int c = 0; c < 3; ++c)
                        row[x * 4 + c] = ci[c] >= 0 ? p[ci[c]] : 0.0f;
                    row[x * 4 + 3] = ci[3] >= 0 ? p[ci[3]] : 1.0f;
                }
            }
        }
    }

    if (!readOk) {
        const std::string why = in->geterror();
        in->close();
        error = QString("Failed to decode EXR pixels: %1 (%2)")
                    .arg(path, QString::fromStdString(why));
        return false;
    }
    const QString compression =
        QString::fromStdString(spec.get_string_attribute("compression", ""));
    in->close();

    out = LoadedImageInfo{};
    out.filePath = path;
    out.fileName = QFileInfo(path).fileName();
    out.extension = "exr";
    out.width = width;
    out.height = height;
    out.channels = nchannels;
    out.buffer = std::move(buffer);
    out.passes = passes;
    out.activePass = active;
    out.compression = compression;
    out.activeRawNames = pass.rawNames;
    error.clear();
    return true;
#else
    Q_UNUSED(path);
    Q_UNUSED(out);
    error = "EXR support not enabled (OpenImageIO not found at build time).";
    return false;
#endif
}

} // namespace trace::core
