#include "core/StillImageLoader.h"

#include "core/SeqProfile.h"

#include <algorithm>
#include <cmath>
#include <memory>
#include <optional>

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


// ARE TWO PASSES THE SAME PICTURE? Measured from a band of scanlines, never
// inferred from the names.
//
// The 27-channel Redshift file writes its beauty render twice -- once as the
// root R,G,B and once as a named `Beauty` layer -- and showing the same image
// under two names with nothing said about it is the kind of quiet confusion a
// review tool exists to remove.
//
// BIT-EQUALITY WOULD FIND NOTHING, and that is the whole difficulty. The two
// copies are compressed INDEPENDENTLY with lossy DWAA, so only 5-7% of their
// pixels are bit-identical; the measured difference is a mean absolute 0.003 to
// 0.005 on values whose mean is about 1.0, with a maximum around 0.05. So the
// test is a RELATIVE mean absolute difference against a tolerance, and the
// measured figure is carried to the HUD so the claim can be checked rather than
// believed.
//
// ONE READ COVERS EVERY PASS. A band across ALL channels is a few MB at 1080p
// (1920 x 24 x 27 x 4 = 5.0 MB), where a band per pass would be a read per pass;
// the comparison is then done in memory.
//
// TWO PASSES THAT ARE BOTH EMPTY ARE NOT CALLED DUPLICATES. A relative measure
// is meaningless when both sides are ~0, and two AOVs that happen to be black in
// the sampled frame are a property of that frame rather than of the render --
// exactly the kind of per-frame accident this must not assert for a sequence.
constexpr int kDuplicateBandRows = 24;
constexpr float kDuplicateRelTolerance = 0.02f;   // 2% of the signal
constexpr float kDuplicateMinSignal = 1e-4f;

void detectDuplicatePasses(OIIO::ImageInput& in, const OIIO::ImageSpec& spec,
                           std::vector<ExrPass>& passes) {
    const int width = spec.width;
    const int height = spec.height;
    const int nchannels = spec.nchannels;
    if (width <= 0 || height <= 0 || nchannels <= 0 || passes.size() < 2) return;

    const int rows = std::min(kDuplicateBandRows, height);
    const int y0 = std::max(0, height / 2 - rows / 2);
    std::vector<float> band(static_cast<std::size_t>(width) * static_cast<std::size_t>(rows) *
                            static_cast<std::size_t>(nchannels));
    if (!in.read_scanlines(0, 0, y0, y0 + rows, 0, 0, nchannels, OIIO::TypeDesc::FLOAT,
                           band.data()))
        return;

    const std::size_t samples = static_cast<std::size_t>(width) * static_cast<std::size_t>(rows);

    // Mean absolute value per pass, so "is there any signal here" is answered
    // before any pair is compared.
    std::vector<double> signal(passes.size(), 0.0);
    for (std::size_t p = 0; p < passes.size(); ++p) {
        double sum = 0.0;
        int used = 0;
        for (int k = 0; k < 3; ++k) {
            const int ch = passes[p].channel[k];
            if (ch < 0 || ch >= nchannels) continue;
            ++used;
            for (std::size_t s = 0; s < samples; ++s)
                sum += std::abs(static_cast<double>(band[s * nchannels + ch]));
        }
        signal[p] = used > 0 ? sum / (static_cast<double>(samples) * used) : 0.0;
    }

    for (std::size_t i = 1; i < passes.size(); ++i) {
        for (std::size_t j = 0; j < i; ++j) {
            // Only like against like: a position pass and a colour pass are not
            // the same picture even if their numbers happen to be close.
            if (passes[i].cls != passes[j].cls) continue;
            if (passes[i].channelCount() != passes[j].channelCount()) continue;
            if (!passes[j].duplicateOf.isEmpty()) continue;   // chain to the original
            if (signal[i] < kDuplicateMinSignal && signal[j] < kDuplicateMinSignal) continue;

            double diff = 0.0;
            int used = 0;
            bool comparable = true;
            for (int k = 0; k < 3; ++k) {
                const int a = passes[i].channel[k];
                const int b = passes[j].channel[k];
                if (a < 0 || b < 0 || a >= nchannels || b >= nchannels) {
                    if ((a < 0) != (b < 0)) comparable = false;
                    continue;
                }
                ++used;
                for (std::size_t s = 0; s < samples; ++s)
                    diff += std::abs(static_cast<double>(band[s * nchannels + a]) -
                                     static_cast<double>(band[s * nchannels + b]));
            }
            if (!comparable || used == 0) continue;

            const double mad = diff / (static_cast<double>(samples) * used);
            const double scale = std::max({signal[i], signal[j], 1e-6});
            const double rel = mad / scale;
            if (rel > kDuplicateRelTolerance) continue;

            // Named after the EARLIER pass, which for the root-versus-Beauty
            // case is the root -- the composite the file is "of".
            passes[i].duplicateOf =
                QString("%1 (%2%)")
                    .arg(passes[j].displayName,
                         QString::number(rel * 100.0, 'f', rel * 100.0 < 1.0 ? 2 : 1));
            break;
        }
    }
}
} // namespace
#endif

bool StillImageLoader::loadExr(const QString& path, LoadedImageInfo& out, QString& error) const {
#ifdef TRACE_WITH_OIIO
    namespace oiio = OIIO;

    namespace prof = ::trace::core::seqprofile;
    std::unique_ptr<oiio::ImageInput> in;
    {
        // REPEATED PER-FRAME OPEN. Every frame of a sequence re-opens the file
        // and re-reads its header; nothing is carried between frames. Timed
        // separately because "setup work repeated per frame" is a named
        // suspect, not because it is assumed to be large.
        prof::Scope g{prof::Stage::Open};
        in = oiio::ImageInput::open(path.toStdString());
    }
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

    std::optional<prof::Scope> groupScope;
    if (prof::enabled()) groupScope.emplace(prof::Stage::Group);

    QStringList names;
    names.reserve(nchannels);
    for (int c = 0; c < nchannels; ++c)
        names << QString::fromStdString(spec.channelnames[static_cast<std::size_t>(c)]);

    std::vector<ExrPass> passes = groupExrChannels(names);

    // DUPLICATES ARE MEASURED ONCE PER CHANNEL LAYOUT. The key is the file's own
    // channel names, which every frame of a sequence shares, so the band read
    // happens on the first frame and the answer is reused for the rest.
    const QString dupKey = names.join(QLatin1Char('\n'));
    if (dupKey != duplicateKey_) {
        detectDuplicatePasses(*in, spec, passes);
        duplicateKey_ = dupKey;
        duplicateLabels_.clear();
        for (const ExrPass& p : passes) duplicateLabels_ << p.duplicateOf;
    } else if (duplicateLabels_.size() == static_cast<int>(passes.size())) {
        for (std::size_t i = 0; i < passes.size(); ++i)
            passes[i].duplicateOf = duplicateLabels_.at(static_cast<int>(i));
    }

    const int active = choosePass(passes, preferredPass_);
    if (active < 0) {
        in->close();
        error = QString("No displayable channels in EXR: %1").arg(path);
        return false;
    }
    const ExrPass& pass = passes[static_cast<std::size_t>(active)];
    groupScope.reset();

    // RGBA FLOAT, SCENE-REFERRED, NOT CLAMPED.
    //
    // Four components rather than three so one layout serves every pass and the
    // colour stage has one descriptor to write against. The 25% that costs
    // against an RGB-only layout buys a single code path through the transform,
    // the display mapping and the frame cache.
    std::shared_ptr<FrameBuffer> buffer;
    {
        prof::Scope g{prof::Stage::Alloc};
        buffer = FrameBuffer::allocate(width, height, PixelLayout::RGBAF32);
    }
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
            //
            // Timed separately because it is a FULL PASS over the whole float
            // buffer (w*h writes, first-touching every page of a 33 MB
            // allocation at 1080p) to set one component.
            prof::Scope g{prof::Stage::AlphaFill};
            for (int y = 0; y < height; ++y) {
                float* row = reinterpret_cast<float*>(base + static_cast<std::size_t>(y) * stride);
                for (int x = 0; x < width; ++x) row[x * 4 + 3] = 1.0f;
            }
        }
        prof::Scope g{prof::Stage::Read};
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
        {
            prof::Scope g{prof::Stage::Read};
            readOk = in->read_image(0, 0, begin, end, oiio::TypeDesc::FLOAT, tmp.data());
        }
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
    prof::Scope tailScope{prof::Stage::Tail};
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
