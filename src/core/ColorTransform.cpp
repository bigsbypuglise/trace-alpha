#include "core/ColorTransform.h"

#include "core/ParallelBands.h"

#include <QFileInfo>

#include <atomic>
#include <cstddef>
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
    // The SAME transform, compiled for a float source. Two processors rather
    // than one because the optimisation and the internal ops are chosen from the
    // bit depths at build time, which is the whole reason getOptimizedCPUProcessor
    // takes them -- asking the uint8 processor to read floats is not a thing it
    // can do. Video keeps the uint8 one untouched, byte for byte, so nothing
    // measured on the video path moves because EXR gained a float source.
    OCIO::ConstCPUProcessorRcPtr cpuFloat;
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

bool ColorTransform::hasFloatProcessor() const {
#ifdef TRACE_WITH_OCIO
    return impl_ && static_cast<bool>(impl_->cpuFloat);
#else
    return false;
#endif
}

void ColorTransform::reset() {
    config_ = Config{};
    enabled_ = false;
#ifdef TRACE_WITH_OCIO
    if (impl_) {
        impl_->cpu.reset();
        impl_->cpuFloat.reset();
    }
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
        if (impl_) {
            impl_->cpu.reset();
            impl_->cpuFloat.reset();
        }
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

        // TWO PROCESSORS FOR ONE TRANSFORM, BECAUSE THERE ARE TWO KINDS OF
        // SOURCE AND EXACTLY ONE KIND OF DISPLAY.
        //
        // uint8 -> uint8 is the VIDEO path, unchanged from stage 1 and
        // deliberately so: a decoded video frame at this seam already is an
        // 8-bit BGRA display buffer, so a float pipeline over it would add two
        // conversions and recover nothing the source ever had.
        //
        // f32 -> uint8 is the EXR path, and it is where stage 1's recorded
        // 8-bit limit is lifted. A scene-linear EXR is not display-referred and
        // is not bounded at 1.0 -- measured on the Redshift beauty pass, 48% of
        // red, 42% of green and 66% of blue samples exceed it -- so the old
        // arrangement handed the view transform a picture whose highlights had
        // already been clipped by loadExr. The output stays uint8 because that
        // is what both renderers present and what the panel can show; what
        // changed is that the clip now happens at the END of the chain.
        //
        // 10-bit output and HDR remain formally deferred behind their own two
        // external gates and are not what this buys.
        impl_->cpu = processor->getOptimizedCPUProcessor(
            OCIO::BIT_DEPTH_UINT8, OCIO::BIT_DEPTH_UINT8,
            OCIO::OPTIMIZATION_DEFAULT);
        if (!impl_->cpu) {
            error = QStringLiteral("OpenColorIO produced no CPU processor.");
            return false;
        }
        impl_->cpuFloat = processor->getOptimizedCPUProcessor(
            OCIO::BIT_DEPTH_F32, OCIO::BIT_DEPTH_UINT8,
            OCIO::OPTIMIZATION_DEFAULT);
        if (!impl_->cpuFloat) {
            // Not fatal: video still works. But it must not be silent, and
            // hasFloatProcessor() is what the EXR path asks so it cannot show
            // an untransformed picture while the HUD claims a transform is on.
            error = QStringLiteral(
                "OpenColorIO produced no float CPU processor; EXR sources will "
                "show their default display mapping.");
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

    // TWO SOURCE LAYOUTS, ONE DESTINATION. Planar YUV is declined here as it
    // always was, and MainWindow keeps planar output off while the stage is
    // active so that is not a path a user lands on.
    const bool floatSource = isFloatRgba(in.buffer->layout());
    if (!floatSource && in.buffer->layout() != PixelLayout::BGRA8) return false;
    if (floatSource && !hasFloatProcessor()) return false;

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
    // The band arithmetic itself now lives in ParallelBands.h so this stage and
    // the float display mapping cannot drift apart on it.
    const uint8_t* srcBase = in.buffer->data();
    const int srcStride = in.buffer->bytesPerLine();
    const int dstStride = dst->bytesPerLine();

    std::atomic<bool> ok{true};
    // The source descriptor is the only thing that differs between a video
    // frame and an EXR pass: BGRA bytes against RGBA floats. The destination is
    // BGRA8 either way, because that is what both renderers present.
    const auto ordering = floatSource ? OCIO::CHANNEL_ORDERING_RGBA
                                      : OCIO::CHANNEL_ORDERING_BGRA;
    const auto depth = floatSource ? OCIO::BIT_DEPTH_F32 : OCIO::BIT_DEPTH_UINT8;
    const OCIO::ConstCPUProcessorRcPtr& cpu = floatSource ? impl_->cpuFloat : impl_->cpu;

    auto runBand = [&](int y0, int rows) {
        if (rows <= 0) return;
        try {
            OCIO::PackedImageDesc srcDesc(
                const_cast<uint8_t*>(srcBase + static_cast<std::size_t>(y0) * srcStride),
                static_cast<long>(w), static_cast<long>(rows),
                ordering, depth,
                OCIO::AutoStride, OCIO::AutoStride,
                static_cast<ptrdiff_t>(srcStride));
            OCIO::PackedImageDesc dstDesc(
                out8 + static_cast<std::size_t>(y0) * dstStride,
                static_cast<long>(w), static_cast<long>(rows),
                OCIO::CHANNEL_ORDERING_BGRA, OCIO::BIT_DEPTH_UINT8,
                OCIO::AutoStride, OCIO::AutoStride,
                static_cast<ptrdiff_t>(dstStride));
            cpu->apply(srcDesc, dstDesc);
        } catch (const std::exception&) {
            ok.store(false, std::memory_order_relaxed);
        }
    };

    runInRowBands(h, runBand);
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
