#include "core/ColorTransform.h"

#include <QFileInfo>

#include <atomic>
#include <thread>
#include <vector>

#ifdef TRACE_WITH_OCIO
#include <OpenColorIO/OpenColorIO.h>
namespace OCIO = OCIO_NAMESPACE;
#endif

namespace trace::core {

struct ColorTransform::Impl {
#ifdef TRACE_WITH_OCIO
    // The compiled result, and the ONLY thing the apply path touches. Held as
    // the CPU processor rather than the Processor so the optimisation and the
    // bit depths are decided once at compile time instead of per frame.
    OCIO::ConstCPUProcessorRcPtr cpu;
#endif
};

ColorTransform::ColorTransform() : impl_(std::make_unique<Impl>()) {}
ColorTransform::~ColorTransform() = default;

bool ColorTransform::available() {
#ifdef TRACE_WITH_OCIO
    return true;
#else
    return false;
#endif
}

QString ColorTransform::ocioVersion() {
#ifdef TRACE_WITH_OCIO
    return QString::fromLatin1(OCIO::GetVersion());
#else
    return QString();
#endif
}

bool ColorTransform::hasProcessor() const {
#ifdef TRACE_WITH_OCIO
    return impl_ && static_cast<bool>(impl_->cpu);
#else
    return false;
#endif
}

void ColorTransform::reset() {
    config_ = Config{};
    enabled_ = false;
#ifdef TRACE_WITH_OCIO
    if (impl_) impl_->cpu.reset();
#endif
}

QString ColorTransform::description() const {
    switch (config_.kind) {
        case Kind::None: return QString();
        case Kind::Lut: return QFileInfo(config_.lutPath).fileName();
        case Kind::DisplayView:
            return QStringLiteral("%1 / %2").arg(config_.display, config_.view);
    }
    return QString();
}

bool ColorTransform::setConfig(const Config& config, QString& error) {
    error.clear();

    if (config.kind == Kind::None) {
        config_ = config;
#ifdef TRACE_WITH_OCIO
        if (impl_) impl_->cpu.reset();
#endif
        return true;
    }

#ifndef TRACE_WITH_OCIO
    error = QStringLiteral(
        "Colour management is not available: this build was compiled without "
        "OpenColorIO.");
    return false;
#else
    try {
        OCIO::ConstProcessorRcPtr processor;

        if (config.kind == Kind::Lut) {
            const QFileInfo fi(config.lutPath);
            if (!fi.exists() || !fi.isFile()) {
                error = QStringLiteral("LUT not found: %1").arg(config.lutPath);
                return false;
            }
            // A .cube IS an OCIO FileTransform. No config file is involved and
            // none is invented: CreateRaw() is a minimal config that exists
            // only to own the transform, so a LUT loads with no colour
            // management set up at all.
            auto ft = OCIO::FileTransform::Create();
            ft->setSrc(config.lutPath.toStdString().c_str());
            ft->setInterpolation(OCIO::INTERP_BEST);
            processor = OCIO::Config::CreateRaw()->getProcessor(ft);
        } else {
            // Kind::DisplayView. No UI reaches this in stage 1; it is compiled
            // and reachable so the dialog is a call site later.
            auto cfg = config.configPath.isEmpty()
                           ? OCIO::Config::CreateFromEnv()
                           : OCIO::Config::CreateFromFile(
                                 config.configPath.toStdString().c_str());

            // THE INPUT SPACE COMES FROM THE scene_linear ROLE WHEN UNSTATED,
            // NEVER FROM getColorSpaceFromFilepath(). Measured on the Redshift
            // config in stage 0: its file rules map any .exr to "Raw", so the
            // obvious call returns a wrong answer while every API call looks
            // correct, and the picture comes out flat.
            std::string input = config.inputSpace.toStdString();
            if (input.empty()) {
                const char* role = cfg->getCanonicalName(OCIO::ROLE_SCENE_LINEAR);
                if (role && *role) input = role;
            }
            if (input.empty()) {
                error = QStringLiteral(
                    "The config states no scene_linear role, so there is no "
                    "safe default input colour space.");
                return false;
            }

            std::string display = config.display.toStdString();
            if (display.empty() && cfg->getDefaultDisplay()) display = cfg->getDefaultDisplay();
            std::string view = config.view.toStdString();
            if (view.empty() && cfg->getDefaultView(display.c_str()))
                view = cfg->getDefaultView(display.c_str());

            auto dvt = OCIO::DisplayViewTransform::Create();
            dvt->setSrc(input.c_str());
            dvt->setDisplay(display.c_str());
            dvt->setView(view.c_str());

            if (!config.look.isEmpty()) {
                auto lvt = OCIO::LookTransform::Create();
                lvt->setLooks(config.look.toStdString().c_str());
                lvt->setSrc(input.c_str());
                lvt->setDst(input.c_str());
                auto group = OCIO::GroupTransform::Create();
                group->appendTransform(lvt);
                group->appendTransform(dvt);
                processor = cfg->getProcessor(group);
            } else {
                processor = cfg->getProcessor(dvt);
            }
        }

        if (!processor) {
            error = QStringLiteral("OpenColorIO produced no processor.");
            return false;
        }

        // UINT8 IN AND OUT, AND THAT IS A STATED LIMIT RATHER THAN AN OVERSIGHT.
        // The frame reaching this stage is already an 8-bit BGRA display buffer
        // -- that is what both renderers present -- so asking OCIO for a
        // float pipeline here would add two conversions and buy nothing back
        // that the buffer can carry. It also means this stage cannot serve a
        // scene-linear ACEScg workflow at full precision: that needs a float
        // display buffer end to end, which is the GPU stage's problem and is
        // recorded as such rather than half-built here.
        impl_->cpu = processor->getOptimizedCPUProcessor(
            OCIO::BIT_DEPTH_UINT8, OCIO::BIT_DEPTH_UINT8,
            OCIO::OPTIMIZATION_DEFAULT);
        if (!impl_->cpu) {
            error = QStringLiteral("OpenColorIO produced no CPU processor.");
            return false;
        }
        config_ = config;
        return true;
    } catch (const std::exception& e) {
        // The PREVIOUS configuration stays in force -- see the header. A LUT
        // that fails to parse must not read as "loaded, and does nothing".
        error = QStringLiteral("OpenColorIO: %1").arg(QString::fromUtf8(e.what()));
        return false;
    }
#endif
}

bool ColorTransform::apply(const VideoFrame& in, VideoFrame& out) const {
#ifndef TRACE_WITH_OCIO
    (void)in; (void)out;
    return false;
#else
    if (!isActive() || in.isNull() || !in.buffer) return false;
    if (in.buffer->layout() != PixelLayout::BGRA8) return false;

    const int w = in.buffer->width();
    const int h = in.buffer->height();
    if (w <= 0 || h <= 0) return false;

    auto dst = FrameBuffer::allocate(w, h, PixelLayout::BGRA8);
    if (!dst) return false;
    uint8_t* out8 = dst->data();

    // SEPARATE SOURCE AND DESTINATION DESCRIPTORS, APPLIED IN PARALLEL ROW BANDS
    // OVER ONE SHARED PROCESSOR.
    //
    // The source buffer is READ ONLY here. That is what keeps Copy Frame copying
    // source pixels and what makes this a display stage rather than a decode
    // one -- and it is not merely tidy: the source is very likely still
    // referenced by the frame cache and by the decoder's recycling pool, so
    // writing into it would corrupt entries other code believes are decoded
    // source. (The first cut memcpy'd the frame and transformed the copy in
    // place, which is an extra pass over 58 MB at 4608x3164 and buys nothing --
    // OCIO takes two descriptors precisely so the caller need not do that.)
    //
    //
    // OCIO's CPUProcessor::apply is single-threaded, and measured that way this
    // stage cost 77.3ms on a 3840x2160 frame -- 9.3 ns/pixel, linear in pixel
    // count (the 4608x3164 Alexa clip read 135.7ms at the same rate) -- which is
    // most of a 41.67ms budget spent twice over and took 4K playback to 47.9% of
    // real time. A ConstCPUProcessor is immutable once built and is safe to
    // apply from several threads at once, which is exactly what OIIO's own
    // colour path does, so the bands share one processor and own disjoint rows.
    //
    // Bands are ROW RANGES, so each is a contiguous sub-image and its descriptor
    // is the same descriptor with a different base pointer and height. No tile
    // seams are possible: a display transform is per-pixel, so a row band is
    // exactly independent.
    const unsigned hw = std::thread::hardware_concurrency();
    int bands = static_cast<int>(hw == 0 ? 1u : hw);
    if (bands > 16) bands = 16;              // past this the per-frame thread
                                             // cost stops paying for itself
    const int kMinRowsPerBand = 64;          // and a small picture is not worth
    if (bands > h / kMinRowsPerBand) bands = h / kMinRowsPerBand;
    if (bands < 1) bands = 1;

    const uint8_t* srcBase = in.buffer->data();
    const int srcStride = in.buffer->bytesPerLine();
    const int dstStride = dst->bytesPerLine();

    std::atomic<bool> ok{true};
    auto runBand = [&](int y0, int rows) {
        if (rows <= 0) return;
        try {
            OCIO::PackedImageDesc srcDesc(
                const_cast<uint8_t*>(srcBase + static_cast<size_t>(y0) * srcStride),
                static_cast<long>(w), static_cast<long>(rows),
                OCIO::CHANNEL_ORDERING_BGRA, OCIO::BIT_DEPTH_UINT8,
                OCIO::AutoStride, OCIO::AutoStride,
                static_cast<ptrdiff_t>(srcStride));
            OCIO::PackedImageDesc dstDesc(
                out8 + static_cast<size_t>(y0) * dstStride,
                static_cast<long>(w), static_cast<long>(rows),
                OCIO::CHANNEL_ORDERING_BGRA, OCIO::BIT_DEPTH_UINT8,
                OCIO::AutoStride, OCIO::AutoStride,
                static_cast<ptrdiff_t>(dstStride));
            impl_->cpu->apply(srcDesc, dstDesc);
        } catch (const std::exception&) {
            ok.store(false, std::memory_order_relaxed);
        }
    };

    if (bands == 1) {
        runBand(0, h);
    } else {
        const int rowsPer = h / bands;
        std::vector<std::thread> workers;
        workers.reserve(static_cast<size_t>(bands) - 1);
        for (int b = 0; b < bands - 1; ++b)
            workers.emplace_back(runBand, b * rowsPer, rowsPer);
        // The calling thread takes the last band, including the remainder rows,
        // rather than idling while N others work.
        runBand((bands - 1) * rowsPer, h - (bands - 1) * rowsPer);
        for (auto& t : workers) t.join();
    }
    if (!ok.load(std::memory_order_relaxed)) return false;

    out = VideoFrame{};
    out.buffer = std::move(dst);
    // Identity, colorimetry and the preview tag are carried across unchanged:
    // this stage changes what the pixels LOOK like and nothing about which
    // frame they are, so every consumer downstream keeps its own meaning.
    out.frameIndex = in.frameIndex;
    out.color = in.color;
    out.previewRes = in.previewRes;
    return true;
#endif
}

} // namespace trace::core
