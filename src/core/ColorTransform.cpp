#include "core/ColorTransform.h"

#include "core/ParallelBands.h"

#include <QFileInfo>

#include <algorithm>
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
    configSource_ = ConfigSource::None;
    configLabel_.clear();
#ifdef TRACE_WITH_OCIO
    if (impl_) {
        impl_->cpu.reset();
        impl_->cpuFloat.reset();
    }
#endif
}

// ---- Config discovery -------------------------------------------------------
//
// THREE SOURCES, ONE RESOLVER, AND EVERY CALLER GOES THROUGH IT. The dialog's
// combo boxes, setConfig() and the selftest all resolve a config the same way,
// so "which config is in force" cannot mean one thing in the enumeration and
// another in the compile -- the choke-point property applyColorTransformToRenderer
// has on the display side.
//
//   an explicit file        -> CreateFromFile(path)        source File
//   an "ocio://..." URI     -> CreateFromFile(uri)         source Builtin
//   nothing, $OCIO set      -> CreateFromFile($OCIO)       source Env
//   nothing, $OCIO unset    -> CreateFromFile(default URI) source Builtin
//
// MEASURED: CreateFromFile takes a builtin URI as happily as a path, so there is
// ONE entry point rather than a branch on whether the string looks like a URI.
//
// AND THE ONE THAT MATTERS: Config::CreateFromEnv() is NOT used, anywhere.
// With $OCIO unset it neither throws nor returns null -- it returns a "Color
// management disabled" RAW config carrying ONE colour space and ONE display, and
// says so only on stderr, which no GUI shows. Stage 1's DisplayView branch
// called it whenever configPath was empty, so an empty path would have compiled
// against that raw config and produced a transform that looks loaded and does
// nothing. That is precisely the identity-versus-working failure the selftest's
// moved-pixel assertion exists to catch, and it is why $OCIO is consulted only
// when it is actually set.

namespace {

#ifdef TRACE_WITH_OCIO
// The URI for the built-in fallback. `ocio://default` is resolved by the Config
// factory; the registry has no getDefaultBuiltinConfigName(), so this cannot be
// looked up and is carried literally.
constexpr const char* kDefaultBuiltinUri = "ocio://default";
#endif

} // namespace

QList<ColorTransform::BuiltinConfig> ColorTransform::builtinConfigs() {
    QList<BuiltinConfig> out;
#ifdef TRACE_WITH_OCIO
    try {
        const auto& reg = OCIO::BuiltinConfigRegistry::Get();
        const std::size_t n = reg.getNumBuiltinConfigs();
        for (std::size_t i = 0; i < n; ++i) {
            BuiltinConfig b;
            const char* name = reg.getBuiltinConfigName(i);
            const char* ui = reg.getBuiltinConfigUIName(i);
            if (!name || !*name) continue;
            b.uri = QStringLiteral("ocio://") + QString::fromUtf8(name);
            b.label = (ui && *ui) ? QString::fromUtf8(ui) : b.uri;
            b.recommended = reg.isBuiltinConfigRecommended(i);
            out.push_back(b);
        }
    } catch (const std::exception&) {
        // An empty list is a real answer: the dialog then offers only Browse
        // and whatever $OCIO gives, rather than showing entries that cannot load.
    }
#endif
    // Recommended first, otherwise registry order. Measured on this build: two
    // of eight are flagged recommended, both the v4.0.0/ACES v2.0 pair.
    std::stable_sort(out.begin(), out.end(),
                     [](const BuiltinConfig& a, const BuiltinConfig& b) {
                         return a.recommended && !b.recommended;
                     });
    return out;
}

QString ColorTransform::defaultConfigString(ConfigSource* source) {
#ifdef TRACE_WITH_OCIO
    const QByteArray env = qgetenv("OCIO");
    if (!env.isEmpty()) {
        if (source) *source = ConfigSource::Env;
        return QString::fromLocal8Bit(env);
    }
    if (source) *source = ConfigSource::Builtin;
    return QString::fromLatin1(kDefaultBuiltinUri);
#else
    if (source) *source = ConfigSource::None;
    return QString();
#endif
}

#ifdef TRACE_WITH_OCIO
namespace {

// Resolve a config string to a loaded config, reporting where it came from.
// `error` carries OCIO's own message on failure -- measured text for a missing
// file is "Error could not read '<path>' OCIO profile." -- rather than one
// invented here, so what the dialog shows is what the library said.
OCIO::ConstConfigRcPtr resolveConfig(const QString& configString,
                                     ColorTransform::ConfigSource* source,
                                     QString* label, QString& error) {
    ColorTransform::ConfigSource src = ColorTransform::ConfigSource::File;
    QString s = configString;
    if (s.isEmpty()) {
        s = ColorTransform::defaultConfigString(&src);
    } else if (s.startsWith(QLatin1String("ocio://"))) {
        src = ColorTransform::ConfigSource::Builtin;
    }
    if (s.isEmpty()) {
        error = QStringLiteral("No OpenColorIO config is available.");
        return {};
    }
    try {
        auto cfg = OCIO::Config::CreateFromFile(s.toStdString().c_str());
        if (!cfg) {
            error = QStringLiteral("The config loaded as null: %1").arg(s);
            return {};
        }
        if (source) *source = src;
        if (label) {
            switch (src) {
                case ColorTransform::ConfigSource::Builtin:
                    *label = QStringLiteral("built-in %1").arg(s);
                    break;
                case ColorTransform::ConfigSource::Env:
                    *label = QStringLiteral("$OCIO %1").arg(QFileInfo(s).fileName());
                    break;
                default:
                    *label = QFileInfo(s).fileName();
                    break;
            }
        }
        return cfg;
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
        return {};
    }
}

} // namespace
#endif

QStringList ColorTransform::colorSpaces(const QString& configString, QString& error) {
    error.clear();
    QStringList out;
#ifdef TRACE_WITH_OCIO
    auto cfg = resolveConfig(configString, nullptr, nullptr, error);
    if (!cfg) return out;
    try {
        const int n = cfg->getNumColorSpaces();
        for (int i = 0; i < n; ++i) {
            const char* name = cfg->getColorSpaceNameByIndex(i);
            if (name && *name) out << QString::fromUtf8(name);
        }
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
#else
    error = QStringLiteral("This build was compiled without OpenColorIO.");
    Q_UNUSED(configString);
#endif
    return out;
}

QStringList ColorTransform::displays(const QString& configString, QString& error) {
    error.clear();
    QStringList out;
#ifdef TRACE_WITH_OCIO
    auto cfg = resolveConfig(configString, nullptr, nullptr, error);
    if (!cfg) return out;
    try {
        const int n = cfg->getNumDisplays();
        for (int i = 0; i < n; ++i) {
            const char* name = cfg->getDisplay(i);
            if (name && *name) out << QString::fromUtf8(name);
        }
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
#else
    error = QStringLiteral("This build was compiled without OpenColorIO.");
    Q_UNUSED(configString);
#endif
    return out;
}

QStringList ColorTransform::views(const QString& configString, const QString& display,
                                  QString& error) {
    error.clear();
    QStringList out;
#ifdef TRACE_WITH_OCIO
    auto cfg = resolveConfig(configString, nullptr, nullptr, error);
    if (!cfg) return out;
    try {
        const std::string d = display.toStdString();
        const int n = cfg->getNumViews(d.c_str());
        for (int i = 0; i < n; ++i) {
            const char* name = cfg->getView(d.c_str(), i);
            if (name && *name) out << QString::fromUtf8(name);
        }
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
#else
    error = QStringLiteral("This build was compiled without OpenColorIO.");
    Q_UNUSED(configString);
    Q_UNUSED(display);
#endif
    return out;
}

// THE INPUT DEFAULT, AND IT IS THE scene_linear ROLE. Never
// getColorSpaceFromFilepath(): measured on BOTH configs in the asset set, the
// file rules give a DIFFERENT answer from the role, and both wrong answers are
// the plausible kind.
//
//   Redshift config.ocio :  role ACEScg   file rules 'Raw'
//   ocio://default       :  role ACEScg   file rules 'ACES2065-1'
//
// 'Raw' shows as a flat, un-tone-mapped picture. 'ACES2065-1' is the harder one
// -- it IS scene-linear, so the picture looks plausible and is simply wrong in
// its primaries, i.e. wrong saturation with correct-looking contrast. Neither
// call fails, so nothing but this rule separates them.
QString ColorTransform::sceneLinearSpace(const QString& configString, QString& error) {
    error.clear();
#ifdef TRACE_WITH_OCIO
    auto cfg = resolveConfig(configString, nullptr, nullptr, error);
    if (!cfg) return QString();
    try {
        const char* role = cfg->getCanonicalName(OCIO::ROLE_SCENE_LINEAR);
        if (role && *role) return QString::fromUtf8(role);
        error = QStringLiteral(
            "The config states no scene_linear role, so there is no safe "
            "default input colour space.");
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
#else
    error = QStringLiteral("This build was compiled without OpenColorIO.");
    Q_UNUSED(configString);
#endif
    return QString();
}

QString ColorTransform::defaultDisplay(const QString& configString, QString& error) {
    error.clear();
#ifdef TRACE_WITH_OCIO
    auto cfg = resolveConfig(configString, nullptr, nullptr, error);
    if (!cfg) return QString();
    try {
        const char* d = cfg->getDefaultDisplay();
        if (d && *d) return QString::fromUtf8(d);
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
#else
    error = QStringLiteral("This build was compiled without OpenColorIO.");
    Q_UNUSED(configString);
#endif
    return QString();
}

QString ColorTransform::defaultView(const QString& configString, const QString& display,
                                    QString& error) {
    error.clear();
#ifdef TRACE_WITH_OCIO
    auto cfg = resolveConfig(configString, nullptr, nullptr, error);
    if (!cfg) return QString();
    try {
        const std::string d = display.toStdString();
        const char* v = cfg->getDefaultView(d.c_str());
        if (v && *v) return QString::fromUtf8(v);
    } catch (const std::exception& e) {
        error = QString::fromUtf8(e.what());
    }
#else
    error = QStringLiteral("This build was compiled without OpenColorIO.");
    Q_UNUSED(configString);
    Q_UNUSED(display);
#endif
    return QString();
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
        configSource_ = ConfigSource::None;
        configLabel_.clear();
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
        // Filled in by the DisplayView branch from what the resolver actually
        // loaded, and published only on success -- so a failed setConfig leaves
        // the previous configuration AND its reported source in force, which is
        // the guarantee the header makes.
        ConfigSource resolvedSource = ConfigSource::None;
        QString resolvedLabel;

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
            // Kind::DisplayView -- the dialog's configuration, stage 3.
            //
            // Through the SHARED resolver, not Config::CreateFromEnv(). Stage 1
            // called CreateFromEnv() whenever configPath was empty, and with
            // $OCIO unset that returns a "Color management disabled" RAW config
            // -- one colour space, one display, no throw, no null, an info line
            // on stderr that no GUI shows. An empty path would therefore have
            // compiled a transform that looks loaded and does nothing.
            // Measured with scripts/measure/ocioprobe.
            ColorTransform::ConfigSource src = ConfigSource::None;
            QString label;
            auto cfg = resolveConfig(config.configPath, &src, &label, error);
            if (!cfg) return false;
            resolvedSource = src;
            resolvedLabel = label;

            // THE INPUT SPACE COMES FROM THE scene_linear ROLE WHEN UNSTATED,
            // NEVER FROM getColorSpaceFromFilepath(). Measured on BOTH configs
            // in the asset set, and the file rules disagree with the role on
            // both: the Redshift config answers 'Raw' for .exr where the role is
            // ACEScg, and ocio://default answers 'ACES2065-1' where the role is
            // also ACEScg. The second is the dangerous one -- it IS scene
            // linear, so the picture looks plausible and is simply wrong in its
            // primaries, and every API call along the way succeeds.
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
        configSource_ = resolvedSource;
        configLabel_ = resolvedLabel;
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
