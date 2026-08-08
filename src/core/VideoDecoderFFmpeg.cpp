#include "core/VideoDecoderFFmpeg.h"

#ifdef TRACE_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
}
#endif

#include <cmath>
#include <algorithm>
#include <deque>

#include <QElapsedTimer>
#include <QByteArray>
#include <QFile>
#include <QDir>

namespace trace::core {

#ifdef TRACE_WITH_FFMPEG
namespace {

int swsFlagsFor(bool fast) {
    // Fast: cheapest conversion path for real-time playback/scrubbing.
    // Accurate: full chroma interpolation + accurate rounding for paused
    // frame inspection (Step mode), where exactness matters more than speed.
    return fast ? SWS_FAST_BILINEAR
                : (SWS_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND);
}

// ProRes 4444 decodes to yuva444p12le: a full-resolution 12-bit alpha plane
// alongside Y/U/V. The viewer draws QImage::Format_RGB32, which ignores
// alpha entirely, so every byte swscale spends scaling that plane is thrown
// away — and at 4K it is a quarter of the input data.
//
// For PLANAR formats the alpha plane is a separate plane, so describing the
// same buffer as its alpha-less equivalent is layout-safe: planes 0-2 are
// untouched and plane 3 simply never gets read. Packed formats (rgba and
// friends) interleave alpha per pixel, so stripping would change the layout
// and is deliberately not attempted.
AVPixelFormat alphaStrippedFormat(AVPixelFormat fmt) {
    const AVPixFmtDescriptor* src = av_pix_fmt_desc_get(fmt);
    if (!src) return fmt;
    if (!(src->flags & AV_PIX_FMT_FLAG_ALPHA)) return fmt;
    if (!(src->flags & AV_PIX_FMT_FLAG_PLANAR)) return fmt;
    if (src->flags & AV_PIX_FMT_FLAG_PAL) return fmt;
    if (src->nb_components < 2) return fmt;

    for (const AVPixFmtDescriptor* cand = av_pix_fmt_desc_next(nullptr);
         cand != nullptr;
         cand = av_pix_fmt_desc_next(cand)) {
        if (cand->flags & AV_PIX_FMT_FLAG_ALPHA) continue;
        if (!(cand->flags & AV_PIX_FMT_FLAG_PLANAR)) continue;
        if (cand->nb_components != src->nb_components - 1) continue;
        if (cand->log2_chroma_w != src->log2_chroma_w) continue;
        if (cand->log2_chroma_h != src->log2_chroma_h) continue;
        if ((cand->flags & AV_PIX_FMT_FLAG_BE) != (src->flags & AV_PIX_FMT_FLAG_BE)) continue;
        if ((cand->flags & AV_PIX_FMT_FLAG_RGB) != (src->flags & AV_PIX_FMT_FLAG_RGB)) continue;
        if (cand->comp[0].depth != src->comp[0].depth) continue;

        const AVPixelFormat id = av_pix_fmt_desc_get_id(cand);
        if (id != AV_PIX_FMT_NONE && sws_isSupportedInput(id)) return id;
    }
    return fmt;
}

// swscale defaults to BT.601 coefficients for every YUV source unless it is
// told otherwise. HD and UHD video is BT.709, so the default silently applies
// the wrong matrix to nearly everything Trace opens: skin tones flatten and
// saturated colour shifts hue. Files also carry a range flag (limited 16-235 vs
// full 0-255) that swscale assumes is limited; a full-range file decoded as
// limited comes out washed out.
int swsCoefficientsFor(AVColorSpace spc, int width, int height, bool* inferred) {
    if (inferred) *inferred = false;
    switch (spc) {
        case AVCOL_SPC_BT709:       return SWS_CS_ITU709;
        case AVCOL_SPC_FCC:         return SWS_CS_FCC;
        case AVCOL_SPC_BT470BG:     return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE170M:   return SWS_CS_SMPTE170M;
        case AVCOL_SPC_SMPTE240M:   return SWS_CS_SMPTE240M;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL:   return SWS_CS_BT2020;
        default: break;
    }
    // Unspecified/unknown: the standard fallback every player uses -- anything
    // HD or larger is BT.709, SD is BT.601.
    if (inferred) *inferred = true;
    return (width >= 1280 || height > 576) ? SWS_CS_ITU709 : SWS_CS_ITU601;
}

// Note SWS_CS_ITU601, SWS_CS_ITU624, SWS_CS_SMPTE170M and SWS_CS_DEFAULT are
// all the same value (5) -- they name one set of coefficients, so only one of
// them can appear here.
const char* swsCoefficientsName(int cs) {
    switch (cs) {
        case SWS_CS_ITU709:     return "bt709";
        case SWS_CS_FCC:        return "fcc";
        case SWS_CS_ITU601:     return "bt601";
        case SWS_CS_SMPTE240M:  return "smpte240m";
        case SWS_CS_BT2020:     return "bt2020";
        default:                return "unknown";
    }
}

// Returns false when swscale rejects the request (it does so for conversions
// with no YUV side, e.g. RGB sources -- nothing to correct there).
bool applyColorspaceDetails(SwsContext* ctx, int coefficients, bool srcFullRange) {
    if (!ctx) return false;
    const int* srcTable = sws_getCoefficients(coefficients);
    const int* dstTable = sws_getCoefficients(SWS_CS_DEFAULT);
    // Destination is RGB, which is always full range.
    return sws_setColorspaceDetails(ctx,
                                    srcTable, srcFullRange ? 1 : 0,
                                    dstTable, 1,
                                    0, 1 << 16, 1 << 16) >= 0;
}

SwsContext* createSwsContext(int srcW, int srcH, AVPixelFormat srcFmt, int dstW, int dstH, AVPixelFormat dstFmt, bool fast) {
    const int flags = swsFlagsFor(fast);

#if LIBSWSCALE_VERSION_INT >= AV_VERSION_INT(6, 7, 100)
    // FFmpeg 5.1+: slice-threaded swscale. threads=0 means auto (per CPU count).
    SwsContext* ctx = sws_alloc_context();
    if (ctx) {
        av_opt_set_int(ctx, "srcw", srcW, 0);
        av_opt_set_int(ctx, "srch", srcH, 0);
        av_opt_set_int(ctx, "src_format", srcFmt, 0);
        av_opt_set_int(ctx, "dstw", dstW, 0);
        av_opt_set_int(ctx, "dsth", dstH, 0);
        av_opt_set_int(ctx, "dst_format", dstFmt, 0);
        av_opt_set_int(ctx, "sws_flags", flags, 0);
        av_opt_set_int(ctx, "threads", 0, 0);
        if (sws_init_context(ctx, nullptr, nullptr) < 0) {
            sws_freeContext(ctx);
            ctx = nullptr;
        }
        if (ctx) return ctx;
    }
#endif

    return sws_getContext(srcW, srcH, srcFmt, dstW, dstH, dstFmt, flags, nullptr, nullptr, nullptr);
}

bool envFlagSet(const char* name) {
    const QByteArray v = qgetenv(name);
    return !v.isEmpty() && v != "0" && v.compare("false", Qt::CaseInsensitive) != 0;
}

// Seek-decision logging (TRACE_SEEK_LOG=1). Measurement scaffolding: writes
// one structured line per seek to %TEMP%\trace_seeklog.txt. Off by default.
bool seekLogEnabled() {
    static const bool on = !qgetenv("TRACE_SEEK_LOG").isEmpty()
                        && qgetenv("TRACE_SEEK_LOG") != "0";
    return on;
}

void seekLogLine(const QString& line) {
    static QFile* f = [] {
        auto* file = new QFile(QDir::tempPath() + "/trace_seeklog.txt");
        file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        return file;
    }();
    if (f && f->isOpen()) {
        f->write(line.toUtf8());
        f->write("\n");
        f->flush();
    }
}

// TRACE_OPEN_LOG=1 appends one structured line per successful open to
// %TEMP%\trace_openlog.txt. Exists so the probe-limit comparison matrix is
// validated against exact values -- fps to six places, exact frame counts,
// time base, colour metadata -- rather than numbers read off a screenshot.
// Off by default.
bool openLogEnabled() {
    static const bool on = !qgetenv("TRACE_OPEN_LOG").isEmpty()
                        && qgetenv("TRACE_OPEN_LOG") != "0";
    return on;
}

void openLogLine(const QString& line) {
    static QFile* f = [] {
        auto* file = new QFile(QDir::tempPath() + "/trace_openlog.txt");
        file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        return file;
    }();
    if (f && f->isOpen()) {
        f->write(line.toUtf8());
        f->write("\n");
        f->flush();
    }
}

int envInt(const char* name, int fallback) {
    const QByteArray v = qgetenv(name);
    if (v.isEmpty()) return fallback;
    bool ok = false;
    const int parsed = v.toInt(&ok);
    return ok ? parsed : fallback;
}

} // namespace
#endif

struct VideoDecoderFFmpeg::Impl {
#ifdef TRACE_WITH_FFMPEG
    AVFormatContext* fmt = nullptr;
    AVCodecContext* codec = nullptr;
    const AVCodec* codecDef = nullptr;
    AVFrame* frame = nullptr;
    AVPacket* pkt = nullptr;
    int streamIndex = -1;
    AVPixelFormat dstPixFmt = AV_PIX_FMT_BGRA;

    // Instrumented, read-only file access behind FFmpeg. Owns the AVIOContext.
    MediaIoSource io;

    // Two persistent conversion contexts rather than one shared context.
    // Full-resolution output (playback, stepping, paused seeks, the exact
    // frame after scrub release) and the reduced-resolution drag preview have
    // different geometry *and* different flags, so a single context was torn
    // down and rebuilt every time the two alternated. That cost ~8-9ms and was
    // paid on every slider release -- exactly when the exact frame is meant to
    // appear. Each slot caches its own parameters and rebuilds only when its
    // own inputs change.
    struct SwsSlot {
        SwsContext* ctx = nullptr;
        int srcW = 0;
        int srcH = 0;
        int dstW = 0;
        int dstH = 0;
        AVPixelFormat srcFmt = AV_PIX_FMT_NONE;
        AVPixelFormat dstFmt = AV_PIX_FMT_NONE;
        bool fast = false;
        long long rebuilds = 0;

        long long lastUsed = 0;

        // Colour details are per-context state, not per-call, so they belong to
        // the slot. They are applied without a full rebuild when they change.
        int coefficients = -1;
        bool srcFullRange = false;

        bool matches(int sw, int sh, int dw, int dh, AVPixelFormat sf, AVPixelFormat df, bool f) const {
            return ctx && srcW == sw && srcH == sh && dstW == dw && dstH == dh
                && srcFmt == sf && dstFmt == df && fast == f;
        }
        void adopt(SwsContext* c, int sw, int sh, int dw, int dh, AVPixelFormat sf, AVPixelFormat df, bool f) {
            ctx = c; srcW = sw; srcH = sh; dstW = dw; dstH = dh;
            srcFmt = sf; dstFmt = df; fast = f;
            coefficients = -1; srcFullRange = false;
        }
        // Cheap next to a rebuild: swscale just recomputes its conversion
        // tables, so a mid-file colour change costs microseconds, not a context
        // teardown.
        void setColor(int cs, bool fullRange) {
            if (coefficients == cs && srcFullRange == fullRange) return;
            // Record the request even when swscale rejects it (it does for
            // conversions with no YUV side, e.g. RGB sources) so a failing
            // configuration is not retried on every single frame.
            coefficients = cs;
            srcFullRange = fullRange;
            applyColorspaceDetails(ctx, cs, fullRange);
        }
        void reset() {
            if (ctx) sws_freeContext(ctx);
            ctx = nullptr;
            srcW = srcH = dstW = dstH = 0;
            srcFmt = dstFmt = AV_PIX_FMT_NONE;
            fast = false;
            coefficients = -1;
            srcFullRange = false;
        }
    };
    // Measurement showed two slots are not enough: three distinct
    // configurations are live during a scrub cycle -- full-res accurate (the
    // exact frame on release), full-res fast (speculative reverse-cache fills
    // during the GOP walk) and half-res fast (the drag preview). Keying two
    // slots on geometry alone left the full slot thrashing between fast and
    // accurate flags, still rebuilding ~12 times over three drags. A small
    // LRU set keyed on the complete parameter tuple holds all of them.
    static constexpr int kSwsSlotCount = 4;
    SwsSlot swsSlots[kSwsSlotCount];
    long long swsUseClock = 0;
    long long swsRebuilds = 0;

    SwsSlot* acquireSwsSlot(int sw, int sh, int dw, int dh,
                            AVPixelFormat sf, AVPixelFormat df, bool fast,
                            bool* rebuilt) {
        for (auto& s : swsSlots) {
            if (s.matches(sw, sh, dw, dh, sf, df, fast)) {
                s.lastUsed = ++swsUseClock;
                if (rebuilt) *rebuilt = false;
                return &s;
            }
        }
        SwsSlot* victim = &swsSlots[0];
        for (auto& s : swsSlots) {
            if (!s.ctx) { victim = &s; break; }
            if (s.lastUsed < victim->lastUsed) victim = &s;
        }
        victim->reset();
        SwsContext* ctx = createSwsContext(sw, sh, sf, dw, dh, df, fast);
        if (rebuilt) *rebuilt = true;
        ++swsRebuilds;
        if (!ctx) return nullptr;
        victim->adopt(ctx, sw, sh, dw, dh, sf, df, fast);
        victim->lastUsed = ++swsUseClock;
        return victim;
    }

    int swsSlotsInUse() const {
        int n = 0;
        for (const auto& s : swsSlots) if (s.ctx) ++n;
        return n;
    }

    // Env overrides for A/B testing; when neither is set, conversion quality
    // is chosen per request mode (fast for Playback/Scrub, accurate for Step).
    bool forceFastConvert = false;
    bool forceAccurateConvert = false;
    // Escape hatch: TRACE_KEEP_ALPHA=1 restores scaling of the alpha plane
    // for A/B measurement.
    bool keepAlpha = false;


    AVRational streamTimeBase{0, 1};
    AVRational fpsQ{24, 1};
    int64_t streamStartTs = 0;
    long long lastDecodedFrame = -1;
    int playbackDirection = 1;

    // Frame-exact seek resolution (long-GOP codecs like H.264): after a
    // seek, the first decoded frame's index is resolved from its PTS instead
    // of being labeled as the requested frame, so decodeUntilTarget keeps
    // decoding forward from the keyframe to the *true* target frame.
    bool seekResolvePending = false;
    long long seekTargetFrame = -1;

    // End-of-stream drain state. Frame-threaded decoders hold frames in
    // flight, so reaching input EOF is not the same as having no frames left.
    // Reset whenever the codec is flushed (seek) or recreated (open).
    bool inputEofReached = false;
    bool drainPacketSent = false;
    bool decoderFullyDrained = false;

    // Seek-decision diagnostics (TRACE_SEEK_LOG=1). Measurement only.
    long long requestCounter = 0;
    long long seekCounter = 0;
    long long prevRequestedFrame = -1;
    int64_t lastDecodedPts = AV_NOPTS_VALUE;
    int64_t prevDecodedPts = AV_NOPTS_VALUE;

    struct CachedFrame {
        long long frame = -1;
        QImage image;
        // Set when the entry was converted at preview resolution -- reduced to
        // the displayed size, or to half, whichever is smaller. Such an entry
        // is fine to *shuttle* through during a drag, which is showing frames
        // at that resolution anyway, but must never serve a Step or an exact
        // landing, where the frame is being inspected. Tagging rather than
        // withholding is what lets 4K backward drags hit the cache at all:
        // previously no Scrub preview was stored, so every backward step at 4K
        // missed and paid a seek plus a GOP walk.
        bool previewRes = false;
    };
    // Eviction is FIFO. LRU (promote-on-hit) was prototyped and measured: on
    // 4K H.264, where the cache actually fills and evicts, it produced an
    // identical hit rate (9/15 either way, 9 promotions) because the scrub
    // working set exceeds capacity rather than a hot subset being evicted.
    // At 1080p nothing is evicted at all, so the policy cannot matter there.
    std::deque<CachedFrame> reverseCache;

    // Conversion targets are recycled. Writing into a QImage the viewer or
    // the reverse cache still references makes bits() detach — a full ~38MB
    // deep copy at 4K, every frame, of data sws_scale is about to overwrite.
    // A pool entry whose only holder is the pool reports isDetached(), so it
    // is safe to write in place. Buffers are reused rather than reallocated
    // so the pages stay committed and warm.
    std::deque<QImage> convertPool;
    int convertPoolLimit = 16;

    QImage* acquireConvertTarget(int w, int h, QImage::Format fmt) {
        for (auto& img : convertPool) {
            if (img.isDetached() && img.width() == w && img.height() == h && img.format() == fmt) {
                return &img;
            }
        }
        // Release unreferenced buffers of the wrong geometry so a resolution
        // or format change doesn't pin the pool at stale sizes.
        for (auto it = convertPool.begin(); it != convertPool.end();) {
            const bool stale = it->isDetached()
                && (it->width() != w || it->height() != h || it->format() != fmt);
            it = stale ? convertPool.erase(it) : it + 1;
        }
        if (static_cast<int>(convertPool.size()) >= convertPoolLimit) return nullptr;
        convertPool.emplace_back(w, h, fmt);
        return &convertPool.back();
    }
    // The cache is budgeted in BYTES, not in entries, because its entries are
    // not all the same size: above 1920px a scrub preview is half resolution
    // and therefore a *quarter* of the footprint of a full-res frame. Counting
    // entries priced every one of them at the full-res worst case, so a 4K file
    // got 6 slots and then sat at 47MB of a 192MB budget while a backward drag
    // missed on almost every frame -- six slots cannot serve a walk back
    // through a thirty-frame GOP however good the hit logic is. Same budget,
    // priced honestly, holds ~24.
    long long reverseCacheBudgetBytes = 192LL * 1024 * 1024;
    // Running total, so a push costs one addition rather than a walk of the
    // whole deque.
    long long reverseCacheBytes = 0;
    // Worst-case entry count at full resolution. No longer the eviction rule,
    // but still the right bound for things that must be sized before any frame
    // exists: the conversion pool, and how far a seek walk may fill.
    int reverseCacheCapacity = 12;
    // -1 = adapt per request from measured conversion cost (see below).
    // Overridden by TRACE_SEEK_CACHE_WINDOW for A/B.
    int seekWalkCacheWindowOverride = -1;
    // How much conversion a *drag* may spend filling the cache on one seek
    // walk, against 18ms for a Step landing. Higher fills more of the GOP per
    // seek (fewer seeks, more hits) at the cost of a longer pause on the frame
    // that pays for it. TRACE_SCRUB_FILL_MS to A/B.
    double scrubWalkCacheBudgetMs = 60.0;
    // Size the viewer draws at, or 0 when unknown. Used only as a *ceiling* on
    // scrub preview conversion, never as a floor: a preview is never made
    // larger than half resolution, and never larger than it will be shown.
    int previewDisplayW = 0;
    int previewDisplayH = 0;
#endif
};

VideoDecoderFFmpeg::VideoDecoderFFmpeg() : impl_(new Impl()) {}

VideoDecoderFFmpeg::~VideoDecoderFFmpeg() {
    close();
    delete impl_;
}

bool VideoDecoderFFmpeg::isOpen() const {
#ifdef TRACE_WITH_FFMPEG
    return impl_ && impl_->codec != nullptr;
#else
    return false;
#endif
}

void VideoDecoderFFmpeg::clearForwardQueue() {
    // The synchronous forward-fill queue was removed: it decoded several
    // frames per timer tick in bursts, causing a rhythmic stutter on 4K
    // ProRes. Playback now decodes exactly one frame per request.
    perfStats_.forwardQueueDepth = 0;
}

void VideoDecoderFFmpeg::setScrubPreviewSize(QSize size) {
#ifdef TRACE_WITH_FFMPEG
    if (!impl_) return;
    // TRACE_PREVIEW_DISPLAY_SIZE=0 pins previews back to the plain half-res
    // rule, so the display-size conversion can be A/B'd on feel and sharpness
    // against the behaviour it replaced without a rebuild.
    static const bool useDisplaySize = envInt("TRACE_PREVIEW_DISPLAY_SIZE", 1) != 0;
    const int w = useDisplaySize ? std::max(0, size.width()) : 0;
    const int h = useDisplaySize ? std::max(0, size.height()) : 0;
    if (w == impl_->previewDisplayW && h == impl_->previewDisplayH) return;
    impl_->previewDisplayW = w;
    impl_->previewDisplayH = h;
    // Preview entries were converted at the old size. They would still draw --
    // the viewer scales whatever it is given -- but a drag would then run
    // through frames of visibly different sharpness, which reads as the picture
    // breaking up rather than as a resize. Drop them and let the drag refill.
    impl_->reverseCache.clear();
    impl_->reverseCacheBytes = 0;
#else
    Q_UNUSED(size);
#endif
}

void VideoDecoderFFmpeg::setPlaybackDirection(int direction) {
#ifdef TRACE_WITH_FFMPEG
    if (!impl_) return;
    impl_->playbackDirection = direction < 0 ? -1 : 1;
#else
    Q_UNUSED(direction);
#endif
}

IoPhaseStats VideoDecoderFFmpeg::ioStats(IoPhase phase) const {
#ifdef TRACE_WITH_FFMPEG
    if (impl_) return impl_->io.stats(phase);
#else
    Q_UNUSED(phase);
#endif
    return {};
}

StorageInfo VideoDecoderFFmpeg::storageInfo() const {
#ifdef TRACE_WITH_FFMPEG
    if (impl_) return impl_->io.storage();
#endif
    return {};
}

void VideoDecoderFFmpeg::setStallPump(MediaIoSource::StallPump pump) {
#ifdef TRACE_WITH_FFMPEG
    if (impl_) impl_->io.setStallPump(std::move(pump));
#else
    Q_UNUSED(pump);
#endif
}

void VideoDecoderFFmpeg::cancelOutstandingIo() {
#ifdef TRACE_WITH_FFMPEG
    if (impl_) impl_->io.cancelOutstanding();
#endif
}

void VideoDecoderFFmpeg::setHandoffTiming(double handoffMs) {
    perfStats_.lastHandoffMs = handoffMs;
    const double sampleCount = perfStats_.samples > 0 ? static_cast<double>(perfStats_.samples) : 1.0;
    perfStats_.avgHandoffMs += (handoffMs - perfStats_.avgHandoffMs) / sampleCount;
}

void VideoDecoderFFmpeg::close() {
#ifdef TRACE_WITH_FFMPEG
    if (!impl_) return;

    if (impl_->frame) av_frame_free(&impl_->frame);
    if (impl_->pkt) av_packet_free(&impl_->pkt);

    for (auto& s : impl_->swsSlots) s.reset();
    impl_->swsUseClock = 0;
    impl_->swsRebuilds = 0;

    if (impl_->codec) avcodec_free_context(&impl_->codec);
    // AVFMT_FLAG_CUSTOM_IO means avformat_close_input leaves ->pb alone, so the
    // I/O source owns and frees it. Order matters: close the format context
    // first, since it may still read through pb while shutting down.
    if (impl_->fmt) avformat_close_input(&impl_->fmt);
    impl_->io.close();

    impl_->streamIndex = -1;
    impl_->streamTimeBase = {0, 1};
    impl_->fpsQ = {24, 1};
    impl_->streamStartTs = 0;
    impl_->lastDecodedFrame = -1;
    impl_->playbackDirection = 1;
    impl_->seekResolvePending = false;
    impl_->seekTargetFrame = -1;
    impl_->reverseCache.clear();
    impl_->reverseCacheBytes = 0;
#endif
    currentFrame_ = -1;
    metadata_ = {};
    perfStats_ = {};
}

bool VideoDecoderFFmpeg::open(const QString& path, QString& error) {
#ifdef TRACE_WITH_FFMPEG
    close();

    QElapsedTimer openTimer;
    openTimer.start();

    // Media is read through our own AVIOContext rather than FFmpeg's file
    // protocol. Measurement first: the buffer size matches FFmpeg's own default
    // exactly, so this describes the read pattern Trace already had. It exists
    // so the pattern can be seen at all -- with the built-in protocol, read
    // count, read size and seek behaviour are invisible from outside FFmpeg.
    if (!impl_->io.open(path, error)) {
        return false;
    }

    impl_->fmt = avformat_alloc_context();
    if (!impl_->fmt) {
        error = "FFmpeg: alloc format context failed";
        impl_->io.close();
        return false;
    }
    impl_->fmt->pb = impl_->io.avio();
    // Tells avformat_close_input not to free a pb it did not create.
    impl_->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;

    // Probe limits. Left at FFmpeg's defaults unless overridden, because
    // shrinking them trades metadata certainty for open speed and that trade
    // has to be validated per format, not assumed. Env knobs exist so the
    // comparison matrix can be run without a rebuild per value:
    //   TRACE_PROBESIZE=<bytes>  TRACE_ANALYZEDURATION=<microseconds>
    if (const int probeOverride = envInt("TRACE_PROBESIZE", 0); probeOverride > 0) {
        av_opt_set_int(impl_->fmt, "probesize", probeOverride, 0);
    }
    if (const int analyzeOverride = envInt("TRACE_ANALYZEDURATION", 0); analyzeOverride > 0) {
        av_opt_set_int(impl_->fmt, "analyzeduration", analyzeOverride, 0);
    }

    // The path is still passed so the demuxer can be probed by extension as
    // well as by content; the bytes come from pb.
    QElapsedTimer demuxTimer;
    demuxTimer.start();
    if (avformat_open_input(&impl_->fmt, path.toUtf8().constData(), nullptr, nullptr) < 0) {
        error = "FFmpeg: unable to open file";
        // On failure avformat_open_input frees the context it was given.
        impl_->fmt = nullptr;
        impl_->io.close();
        return false;
    }
    const double demuxMs = static_cast<double>(demuxTimer.nsecsElapsed()) / 1'000'000.0;

    // Probing is the dominant term in open time everywhere it has been
    // measured, so its I/O is captured as a delta rather than guessed at.
    const IoPhaseStats beforeProbe = impl_->io.stats(IoPhase::Open);

    QElapsedTimer streamInfoTimer;
    streamInfoTimer.start();
    if (avformat_find_stream_info(impl_->fmt, nullptr) < 0) {
        error = "FFmpeg: unable to read stream info";
        close();
        return false;
    }
    const double streamInfoMs = static_cast<double>(streamInfoTimer.nsecsElapsed()) / 1'000'000.0;

    const IoPhaseStats afterProbe = impl_->io.stats(IoPhase::Open);
    perfStats_.probeReads = afterProbe.reads - beforeProbe.reads;
    perfStats_.probeBytes = afterProbe.bytes - beforeProbe.bytes;
    perfStats_.probeSeeks = afterProbe.seeks - beforeProbe.seeks;
    perfStats_.streamCount = static_cast<int>(impl_->fmt->nb_streams);
    {
        int64_t v = 0;
        if (av_opt_get_int(impl_->fmt, "probesize", 0, &v) >= 0) perfStats_.probeSizeLimit = v;
        if (av_opt_get_int(impl_->fmt, "analyzeduration", 0, &v) >= 0) perfStats_.analyzeDurationUs = v;
    }

    impl_->streamIndex = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (impl_->streamIndex < 0) {
        error = "FFmpeg: no video stream";
        close();
        return false;
    }

    AVStream* stream = impl_->fmt->streams[impl_->streamIndex];
    const AVCodecParameters* par = stream->codecpar;

    impl_->codecDef = avcodec_find_decoder(par->codec_id);
    if (!impl_->codecDef) {
        error = "FFmpeg: unsupported codec";
        close();
        return false;
    }

    impl_->codec = avcodec_alloc_context3(impl_->codecDef);
    if (!impl_->codec) {
        error = "FFmpeg: alloc codec context failed";
        close();
        return false;
    }

    if (avcodec_parameters_to_context(impl_->codec, par) < 0) {
        error = "FFmpeg: codec params init failed";
        close();
        return false;
    }

    // Threading mode matters enormously for seek-heavy review use:
    // - Frame threading pipelines ACROSS frames. Throughput is great, but
    //   after every seek+flush the pipeline must refill (~thread-count
    //   packets) before one frame comes out — this made reverse stepping
    //   and scrubbing lag ~100ms per request on 4K ProRes.
    // - Slice threading parallelizes WITHIN one frame, so a cold
    //   single-frame request (scrub, step, reverse) stays fast.
    // Intra-only codecs (ProRes, DNxHD, MJPEG) decode every frame
    // independently: force slice threading. Long-GOP codecs (H.264/HEVC)
    // keep frame threading for playback throughput.
    const AVCodecDescriptor* codecDesc = avcodec_descriptor_get(par->codec_id);
    const bool intraOnly = codecDesc && (codecDesc->props & AV_CODEC_PROP_INTRA_ONLY);
    impl_->codec->thread_count = 0;
    impl_->codec->thread_type = intraOnly ? FF_THREAD_SLICE : (FF_THREAD_FRAME | FF_THREAD_SLICE);

    if (avcodec_open2(impl_->codec, impl_->codecDef, nullptr) < 0) {
        error = "FFmpeg: codec open failed";
        close();
        return false;
    }

    impl_->frame = av_frame_alloc();
    impl_->pkt = av_packet_alloc();
    if (!impl_->frame || !impl_->pkt) {
        error = "FFmpeg: frame/packet alloc failed";
        close();
        return false;
    }

    const int w = impl_->codec->width;
    const int h = impl_->codec->height;

    impl_->forceFastConvert = envFlagSet("TRACE_PERF_FAST_CONVERT");
    impl_->forceAccurateConvert = envFlagSet("TRACE_PERF_ACCURATE_CONVERT");
    impl_->keepAlpha = envFlagSet("TRACE_KEEP_ALPHA");
    // A/B the landing-latency tradeoff: 0 = fastest landing, no free
    // backward steps; 12 = old behavior (full cache, slowest landing).
    // The cache is bounded by bytes; this only derives the worst-case entry
    // count (every entry full resolution) for the things that need a number
    // before any frame has been converted.
    {
        const long long frameBytes = static_cast<long long>(w) * h * 4;
        impl_->reverseCacheBudgetBytes = 192LL * 1024 * 1024;
        impl_->reverseCacheBytes = 0;
        // Worst case: every entry full resolution. Half-res scrub entries cost
        // a quarter of this, so the byte budget will hold about four times as
        // many of them -- which is the point.
        const int byFootprint = frameBytes > 0
            ? static_cast<int>(impl_->reverseCacheBudgetBytes / frameBytes)
            : 12;
        impl_->reverseCacheCapacity = std::clamp(byFootprint, 4, 32);
    }
    // One target per outstanding holder: the reverse cache, the viewer, the
    // frame in flight, plus slack. Buffers here are the same ones the old
    // detach path allocated and threw away each frame, so this is not extra
    // steady-state memory.
    impl_->convertPool.clear();
    // The pool's buffers ARE the cache's images, so it must be able to cover a
    // cache full of the smallest entries the budget allows -- half-res above
    // 1920px, four to a full-res frame -- plus the viewer's copy and the frame
    // in flight. Undersizing it is not a correctness problem (the converter
    // falls back to a private buffer) but it reintroduces a per-frame
    // allocation exactly during a drag, which is when it is least affordable.
    {
        const long long px = static_cast<long long>(w) * h;
        const long long smallest = (w > 1920) ? px : px * 4;
        const int maxEntries = smallest > 0
            ? static_cast<int>(impl_->reverseCacheBudgetBytes / smallest)
            : impl_->reverseCacheCapacity;
        impl_->convertPoolLimit = std::clamp(maxEntries, 4, 128) + 4;
    }
    impl_->inputEofReached = false;
    impl_->drainPacketSent = false;
    impl_->decoderFullyDrained = false;
    impl_->scrubWalkCacheBudgetMs = std::max(0, envInt("TRACE_SCRUB_FILL_MS", 60));
    impl_->seekWalkCacheWindowOverride = envInt("TRACE_SEEK_CACHE_WINDOW", -1);
    if (impl_->seekWalkCacheWindowOverride >= 0) {
        impl_->seekWalkCacheWindowOverride =
            std::min(impl_->seekWalkCacheWindowOverride, impl_->reverseCacheCapacity);
    }
    impl_->dstPixFmt = AV_PIX_FMT_BGRA;

    // Prime the full-resolution slot. The preview slot is built lazily on the
    // first reduced-resolution scrub conversion, and only if scrubbing happens.
    if (!impl_->acquireSwsSlot(w, h, w, h, impl_->codec->pix_fmt, impl_->dstPixFmt,
                               false, nullptr)) {
        error = "FFmpeg: swscale init failed";
        close();
        return false;
    }

    metadata_.width = w;
    metadata_.height = h;
    metadata_.codecName = impl_->codecDef->name ? impl_->codecDef->name : "unknown";

    const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(impl_->codec->pix_fmt);
    perfStats_.srcPixelFormat = srcDesc && srcDesc->name ? QString::fromUtf8(srcDesc->name) : QStringLiteral("unknown");
    perfStats_.srcBitDepth = srcDesc ? av_get_bits_per_pixel(srcDesc) : 0;
    perfStats_.dstPixelFormat = QStringLiteral("RGB32/BGRA");
    perfStats_.experimentalFastPathEnabled = impl_->forceFastConvert;

    AVRational fr = av_guess_frame_rate(impl_->fmt, stream, nullptr);
    if (fr.num <= 0 || fr.den <= 0) fr = AVRational{24, 1};
    impl_->fpsQ = fr;
    metadata_.fps = av_q2d(fr);

    impl_->streamTimeBase = stream->time_base;
    impl_->streamStartTs = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;

    if (stream->nb_frames > 0) metadata_.frameCount = static_cast<long long>(stream->nb_frames);
    else if (impl_->fmt->duration > 0) metadata_.frameCount = static_cast<long long>(std::floor((impl_->fmt->duration / static_cast<double>(AV_TIME_BASE)) * metadata_.fps));

    if (impl_->fmt->duration > 0) metadata_.durationSeconds = impl_->fmt->duration / static_cast<double>(AV_TIME_BASE);

    perfStats_.openMs = static_cast<double>(openTimer.nsecsElapsed()) / 1'000'000.0;
    perfStats_.forwardQueueCapacity = 0;

    // Storage identification and source shape, reported so a bad classification
    // is visible rather than silently changing behaviour.
    const StorageInfo& si = impl_->io.storage();
    perfStats_.sourceStorage = QStringLiteral("%1 (%2)%3")
                                   .arg(si.remote ? "REMOTE" : "local")
                                   .arg(si.reason)
                                   .arg(si.overridden ? " [override]" : "");
    perfStats_.sourceVolume = si.volumeRoot + (si.filesystem.isEmpty()
                                                   ? QString()
                                                   : QStringLiteral(" %1").arg(si.filesystem));
    perfStats_.sourceBytes = impl_->io.fileSize();
    perfStats_.ioBufferBytes = MediaIoSource::bufferSize();
    perfStats_.openClassifyMs = si.classifyMs;
    perfStats_.classifyCached = si.classifyCached;
    perfStats_.openFileMs = si.fileOpenMs;
    perfStats_.openDemuxMs = demuxMs;
    perfStats_.openStreamInfoMs = streamInfoMs;
    if (metadata_.durationSeconds > 0.0 && perfStats_.sourceBytes > 0) {
        perfStats_.sourceBitrateMbps =
            (static_cast<double>(perfStats_.sourceBytes) * 8.0 / 1'000'000.0)
            / metadata_.durationSeconds;
    }

    // Everything a probe-limit change could plausibly degrade, recorded in one
    // line so a faster open can be proven not to have cost correctness.
    if (openLogEnabled()) {
        const int audioIdx = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(par->format));
        openLogLine(QStringLiteral(
            "file=%1\tstorage=%2\tprobesize=%3\tanalyzedur=%4"
            "\tclassifyMs=%5\tcached=%6\tfileopenMs=%7\tdemuxMs=%8\tstreaminfoMs=%9\topenMs=%10"
            "\tprobeReads=%11\tprobeBytes=%12\tprobeSeeks=%13"
            "\tstreams=%14\tvideoIdx=%15\taudioIdx=%16\tcodec=%17"
            "\tw=%18\th=%19\tpixfmt=%20\tfps=%21\tfpsQ=%22/%23\ttb=%24/%25"
            "\tdur=%26\tframes=%27\tcolorspace=%28\trange=%29")
            .arg(QFileInfo(path).fileName())
            .arg(si.remote ? "remote" : "local")
            .arg(perfStats_.probeSizeLimit)
            .arg(perfStats_.analyzeDurationUs)
            .arg(QString::number(si.classifyMs, 'f', 2))
            .arg(si.classifyCached ? 1 : 0)
            .arg(QString::number(si.fileOpenMs, 'f', 2))
            .arg(QString::number(demuxMs, 'f', 2))
            .arg(QString::number(streamInfoMs, 'f', 2))
            .arg(QString::number(perfStats_.openMs, 'f', 2))
            .arg(perfStats_.probeReads)
            .arg(perfStats_.probeBytes)
            .arg(perfStats_.probeSeeks)
            .arg(perfStats_.streamCount)
            .arg(impl_->streamIndex)
            .arg(audioIdx)
            .arg(metadata_.codecName)
            .arg(par->width)
            .arg(par->height)
            .arg(pd && pd->name ? QString::fromUtf8(pd->name) : QStringLiteral("?"))
            .arg(QString::number(metadata_.fps, 'f', 6))
            .arg(impl_->fpsQ.num).arg(impl_->fpsQ.den)
            .arg(impl_->streamTimeBase.num).arg(impl_->streamTimeBase.den)
            .arg(QString::number(metadata_.durationSeconds, 'f', 4))
            .arg(metadata_.frameCount)
            .arg(static_cast<int>(par->color_space))
            .arg(static_cast<int>(par->color_range)));
    }

    error.clear();
    return true;
#else
    Q_UNUSED(path);
    error = "FFmpeg support not enabled at build time.";
    return false;
#endif
}

bool VideoDecoderFFmpeg::decodeFrameAt(long long frameIndex, QImage& outImage, QString& error, RequestMode mode) {
#ifdef TRACE_WITH_FFMPEG
    if (!isOpen()) {
        error = "Video decoder not open";
        return false;
    }

    // Attribute every read this request causes to the right activity. Scrub and
    // Step are both random access as far as storage is concerned; only
    // Playback is the sequential forward pattern we are trying to protect.
    impl_->io.setPhase(mode == RequestMode::Playback ? IoPhase::Playback : IoPhase::Seek);

    const AVRational frameTb{impl_->fpsQ.den, impl_->fpsQ.num};
    frameIndex = std::max<long long>(0, frameIndex);

    // Conversion quality per request: fast while frames are in motion
    // (playback, scrubbing), accurate when the user is inspecting a frame
    // (stepping / paused landing frame). Env vars override for A/B testing.
    bool wantFastConvert = (mode != RequestMode::Step);
    if (impl_->forceFastConvert) wantFastConvert = true;
    if (impl_->forceAccurateConvert) wantFastConvert = false;

    auto reportCacheState = [&]() {
        perfStats_.cacheOccupancy = static_cast<int>(impl_->reverseCache.size());
        perfStats_.cacheBytes = impl_->reverseCacheBytes;
        perfStats_.cacheBudgetBytes = impl_->reverseCacheBudgetBytes;
        // How many more entries of the size currently being stored would fit.
        // Reported rather than fixed, because it depends on whether the drag is
        // filling the cache with half-res previews or full-res frames.
        const long long avg = impl_->reverseCache.empty()
            ? 0
            : impl_->reverseCacheBytes / static_cast<long long>(impl_->reverseCache.size());
        perfStats_.cacheCapacity = avg > 0
            ? static_cast<int>(impl_->reverseCacheBudgetBytes / avg)
            : impl_->reverseCacheCapacity;
    };

    auto pushReverseCache = [&](long long cachedFrame, const QImage& image,
                                bool previewRes = false) {
        if (cachedFrame < 0 || image.isNull()) return;

        if (!impl_->reverseCache.empty() && impl_->reverseCache.back().frame == cachedFrame) {
            // A full-res entry always wins: it can serve every caller, a
            // half-res one cannot. Never downgrade an existing entry.
            if (!previewRes || impl_->reverseCache.back().previewRes) {
                impl_->reverseCacheBytes -= impl_->reverseCache.back().image.sizeInBytes();
                impl_->reverseCache.back().image = image;
                impl_->reverseCache.back().previewRes = previewRes;
                impl_->reverseCacheBytes += image.sizeInBytes();
            }
            return;
        }

        impl_->reverseCache.push_back({cachedFrame, image, previewRes});
        impl_->reverseCacheBytes += image.sizeInBytes();
        ++perfStats_.cacheInserts;
        // Evict by footprint. The last entry is never dropped: it is the one
        // just decoded, and an empty cache is worse than an over-budget one by
        // a single frame.
        while (impl_->reverseCacheBytes > impl_->reverseCacheBudgetBytes
               && impl_->reverseCache.size() > 1) {
            impl_->reverseCacheBytes -= impl_->reverseCache.front().image.sizeInBytes();
            impl_->reverseCache.pop_front();
            ++perfStats_.cacheEvictions;
        }
        reportCacheState();
    };

    // Only a Scrub preview may be served a half-res entry; everything else is
    // inspecting the frame and must get full resolution.
    const bool acceptPreviewRes = (mode == RequestMode::Scrub);
    // What this request will actually produce. Mirrors the resolution branch in
    // convertCurrentFrame; kept as one value so a frame cannot be stored at one
    // resolution and tagged as another.
    const bool producesPreviewRes = (mode == RequestMode::Scrub) && (metadata_.width > 1920);
    // Entries of the size this request stores that fit the byte budget. A
    // preview is a quarter of a full-res frame at worst and much less than that
    // when it is converted to the displayed size, so this has to price the size
    // actually being produced -- assuming the full-res worst case is exactly
    // the mistake the byte budget exists to correct.
    auto entriesThatFit = [&](bool previewRes) {
        long long bytes = static_cast<long long>(metadata_.width) * metadata_.height * 4;
        if (previewRes) {
            bytes /= 4;
            if (impl_->previewDisplayW > 0 && impl_->previewDisplayH > 0) {
                const QSize fit = QSize(metadata_.width, metadata_.height)
                    .scaled(impl_->previewDisplayW, impl_->previewDisplayH, Qt::KeepAspectRatio);
                const long long shown =
                    static_cast<long long>(fit.width()) * fit.height() * 4;
                if (shown > 0) bytes = std::min(bytes, shown);
            }
        }
        if (bytes <= 0) return impl_->reverseCacheCapacity;
        return std::clamp(static_cast<int>(impl_->reverseCacheBudgetBytes / bytes), 1, 256);
    };
    auto tryReverseCache = [&](long long wantedFrame) -> bool {
        ++perfStats_.reverseCacheLookups;
        for (auto it = impl_->reverseCache.begin(); it != impl_->reverseCache.end(); ++it) {
            if (it->frame != wantedFrame) continue;
            if (it->previewRes && !acceptPreviewRes) return false;
            ++perfStats_.reverseCacheHits;
            outImage = it->image;
            currentFrame_ = wantedFrame;
            error.clear();
            reportCacheState();
            return true;
        }
        return false;
    };

    auto frameFromPts = [&](int64_t pts) -> long long {
        if (pts == AV_NOPTS_VALUE) return impl_->lastDecodedFrame + 1;
        const int64_t relPts = pts - impl_->streamStartTs;
        return av_rescale_q_rnd(relPts, impl_->streamTimeBase, frameTb, static_cast<AVRounding>(AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX));
    };

    qint64 decodeNs = 0;
    qint64 convertNs = 0;
    qint64 cacheConvertNs = 0;
    long long walkFrames = 0;
    long long walkCacheConverts = 0;
    // Both set below, before the walk runs; read inside decodeUntilTarget.
    bool didSeek = false;
    long long cacheWindow = 0;
    qint64 convertAllocNs = 0;
    qint64 convertWrapNs = 0;
    qint64 ctxRebuildNs = 0;
    qint64 detachNs = 0;
    long long ctxRebuilds = 0;
    long long convertCalls = 0;
    qint64 swsScaleNs = 0;
    qint64 memcpyNs = 0;

    auto updatePerfStats = [&]() {
        const double decodeMs = static_cast<double>(decodeNs) / 1'000'000.0;
        const double convertMs = static_cast<double>(convertNs) / 1'000'000.0;
        // Total is wall-clock work for this request, so it must include the
        // speculative cache conversions even though "cvt" excludes them.
        const double cacheConvertMs = static_cast<double>(cacheConvertNs) / 1'000'000.0;
        const double totalMs = decodeMs + convertMs + cacheConvertMs;
        const double allocMs = static_cast<double>(convertAllocNs) / 1'000'000.0;
        const double wrapMs = static_cast<double>(convertWrapNs) / 1'000'000.0;
        const double swsMs = static_cast<double>(swsScaleNs) / 1'000'000.0;
        const double memcpyMs = static_cast<double>(memcpyNs) / 1'000'000.0;

        perfStats_.lastDecodeMs = decodeMs;
        perfStats_.lastConvertMs = convertMs;
        perfStats_.lastTotalMs = totalMs;
        perfStats_.lastConvertAllocMs = allocMs;
        perfStats_.lastConvertWrapMs = wrapMs;
        perfStats_.lastSwsScaleMs = swsMs;
        perfStats_.lastMemcpyMs = memcpyMs;
        const double ctxRebuildMs = static_cast<double>(ctxRebuildNs) / 1'000'000.0;
        const double detachMs = static_cast<double>(detachNs) / 1'000'000.0;
        perfStats_.lastCtxRebuildMs = ctxRebuildMs;
        perfStats_.lastDetachMs = detachMs;
        perfStats_.lastCtxRebuilds = ctxRebuilds;
        perfStats_.lastConvertCalls = convertCalls;
        perfStats_.lastWalkFrames = walkFrames;
        perfStats_.lastWalkCacheConverts = walkCacheConverts;
        perfStats_.lastWalkCacheConvertMs = static_cast<double>(cacheConvertNs) / 1'000'000.0;

        ++perfStats_.samples;
        const double n = static_cast<double>(perfStats_.samples);
        perfStats_.avgDecodeMs += (decodeMs - perfStats_.avgDecodeMs) / n;
        perfStats_.avgConvertMs += (convertMs - perfStats_.avgConvertMs) / n;
        perfStats_.avgTotalMs += (totalMs - perfStats_.avgTotalMs) / n;
        perfStats_.avgConvertAllocMs += (allocMs - perfStats_.avgConvertAllocMs) / n;
        perfStats_.avgConvertWrapMs += (wrapMs - perfStats_.avgConvertWrapMs) / n;
        perfStats_.avgCtxRebuildMs += (ctxRebuildMs - perfStats_.avgCtxRebuildMs) / n;
        perfStats_.avgDetachMs += (detachMs - perfStats_.avgDetachMs) / n;
        perfStats_.avgSwsScaleMs += (swsMs - perfStats_.avgSwsScaleMs) / n;
        perfStats_.avgMemcpyMs += (memcpyMs - perfStats_.avgMemcpyMs) / n;
    };

    // isCacheFill no longer changes the conversion -- previews and the fills
    // that back them are made at the same size now -- but it still decides
    // which timer the cost lands in, so "cvt" stays the cost of the frame
    // actually shown rather than that plus speculative fills.
    auto convertCurrentFrame = [&](QImage& image, bool fastOverride = false,
                                   bool isCacheFill = false) {
        ++convertCalls;
        const bool useFast = fastOverride || wantFastConvert;
        const int w = impl_->frame->width > 0 ? impl_->frame->width : impl_->codec->width;
        const int h = impl_->frame->height > 0 ? impl_->frame->height : impl_->codec->height;
        const AVPixelFormat decodedPixFmt = static_cast<AVPixelFormat>(impl_->frame->format);
        const AVPixFmtDescriptor* srcDesc = av_pix_fmt_desc_get(decodedPixFmt);
        perfStats_.srcPixelFormat = srcDesc && srcDesc->name ? QString::fromUtf8(srcDesc->name) : QStringLiteral("unknown");
        perfStats_.srcBitDepth = srcDesc ? av_get_bits_per_pixel(srcDesc) : 0;

        // The destination ignores alpha, so don't pay to scale it.
        const AVPixelFormat srcPixFmt = impl_->keepAlpha ? decodedPixFmt
                                                         : alphaStrippedFormat(decodedPixFmt);
        perfStats_.alphaPlaneSkipped = (srcPixFmt != decodedPixFmt);
        if (perfStats_.alphaPlaneSkipped) {
            perfStats_.srcPixelFormat += QStringLiteral(" (a-skip)");
        }

        // Scrub previews convert at reduced resolution for large sources: the
        // sws conversion dominates per-frame cost at 4K and the viewer scales
        // to fit anyway. The landing frame (Step mode, sent on
        // slider release) is always converted at full resolution.
        int dstW = w;
        int dstH = h;
        // Everything converted to serve a drag is halved above 1920px --- the
        // frame presented AND the frames cached on the way to it. Cache fills
        // used to be forced full-res so an entry could later serve a paused
        // step, but entries carry a previewRes tag now and stepping refuses them
        // anyway, so the full-res conversion was being paid for and then
        // declined. It was expensive enough to be the real limit on backward
        // dragging: the seek-walk fill spends a time budget, so pricing each
        // fill at a full-res 4K convert (~15ms) bought one or two frames where
        // a half-res one (~7ms) buys twice as many.
        //
        // Step and Playback fills are untouched and stay full resolution, so
        // reverse *stepping* still caches frames it can reuse.
        //
        // Threshold is `> 1920`, not `>= 1920`: at exactly 1080p halving is a
        // pessimisation. A full-res convert is 1920x1080 -> 1920x1080, which
        // sws does unscaled; halving adds a 1920->960 resample that costs more
        // than the smaller output saves. Measured on the 1080p validation clip,
        // same file, same session: full-res playback `sws 0.57/0.72ms` against
        // half-res scrub `sws 2.50/5.07ms`. The rule was written for 4K, where
        // conversion really does dominate, and 1080p was being caught by it --
        // which throttled the drag shuttle to roughly a third of its rate and
        // showed a soft preview for the privilege.
        if (mode == RequestMode::Scrub && w > 1920) {
            dstW = (w / 2) & ~1;
            dstH = (h / 2) & ~1;
            // Half resolution is a ceiling, not a target. What the drag
            // actually needs is the size the frame will be drawn at: producing
            // 2048x1152 to show it in a 640x360 widget converts four times the
            // pixels that end up on screen, and then hands the surplus to Qt's
            // raster bilinear, which is the weaker resampler of the two. sws is
            // the dominant per-frame cost at 4K, so this is the cheap half of
            // the frame getting cheaper *and* sharper at the same time.
            //
            // Only ever downward: if the viewer is large, the half-res ceiling
            // still applies, so a fullscreen 4K window is unaffected.
            if (impl_->previewDisplayW > 0 && impl_->previewDisplayH > 0) {
                const QSize fit = QSize(w, h).scaled(
                    impl_->previewDisplayW, impl_->previewDisplayH, Qt::KeepAspectRatio);
                if (fit.width() > 0 && fit.width() < dstW) {
                    dstW = fit.width() & ~1;
                    dstH = fit.height() & ~1;
                }
            }
            dstW = std::max(2, dstW);
            dstH = std::max(2, dstH);
        }

        QElapsedTimer wrapTimer;
        wrapTimer.start();
        bool needRebuild = false;
        Impl::SwsSlot* slot = impl_->acquireSwsSlot(
            w, h, dstW, dstH, srcPixFmt, impl_->dstPixFmt, useFast, &needRebuild);
        if (needRebuild) {
            const qint64 rebuildNs = wrapTimer.nsecsElapsed();
            convertWrapNs += rebuildNs;
            ctxRebuildNs += rebuildNs;
            ++ctxRebuilds;
        }
        // Read the matrix/range off the frame rather than the codec context:
        // the frame is what was actually decoded, and container-level metadata
        // is not always propagated before the first frame arrives.
        AVColorSpace spc = impl_->frame->colorspace;
        if (spc == AVCOL_SPC_UNSPECIFIED) spc = impl_->codec->colorspace;
        AVColorRange range = impl_->frame->color_range;
        if (range == AVCOL_RANGE_UNSPECIFIED) range = impl_->codec->color_range;

        bool matrixInferred = false;
        const int coefficients = swsCoefficientsFor(spc, w, h, &matrixInferred);
        const bool srcFullRange = (range == AVCOL_RANGE_JPEG);
        if (slot && slot->ctx) {
            slot->setColor(coefficients, srcFullRange);
        }
        perfStats_.colorMatrix = QString::fromLatin1(swsCoefficientsName(coefficients));
        perfStats_.colorMatrixInferred = matrixInferred;
        perfStats_.srcFullRange = srcFullRange;

        perfStats_.swsContextReused = !needRebuild;
        perfStats_.experimentalFastPathEnabled = useFast;
        perfStats_.swsSlotRebuilds = impl_->swsRebuilds;
        perfStats_.swsSlotsInUse = impl_->swsSlotsInUse();

        if (!slot || !slot->ctx) {
            return;
        }

        // AV_PIX_FMT_BGRA writes bytes B,G,R,A; on little-endian that is
        // exactly QImage::Format_RGB32's 0xffRRGGBB layout (alpha ignored).
        constexpr QImage::Format kFrameFormat = QImage::Format_RGB32;

        // Convert into a buffer nothing else holds, then hand it out by
        // shallow assignment. Writing straight into `image` would deep-copy
        // the previous frame on every bits() call, since the viewer and the
        // reverse cache still reference it.
        QElapsedTimer allocTimer;
        allocTimer.start();
        QImage* target = impl_->acquireConvertTarget(dstW, dstH, kFrameFormat);
        QImage fallback;
        if (!target) {
            // More outstanding references than the pool expects. Correctness
            // must not depend on pool sizing, so use a private buffer.
            fallback = QImage(dstW, dstH, kFrameFormat);
            target = &fallback;
        }
        convertAllocNs += allocTimer.nsecsElapsed();

        if (target->isNull()) {
            return;
        }

        // QImage::bits() is non-const: it detaches, deep-copying the whole
        // buffer whenever the image is still shared (viewer, reverse cache).
        // Time that separately from the trivial pointer setup it used to be
        // lumped in with, so the cost is attributable rather than guessed at.
        const bool wasShared = !target->isDetached();
        QElapsedTimer detachTimer;
        detachTimer.start();
        uint8_t* const dstBits = target->bits();
        const qint64 thisDetachNs = detachTimer.nsecsElapsed();
        convertWrapNs += thisDetachNs;
        detachNs += thisDetachNs;
        perfStats_.lastImageWasShared = wasShared;

        uint8_t* dstData[4] = { dstBits, nullptr, nullptr, nullptr };
        int dstLinesize[4] = { static_cast<int>(target->bytesPerLine()), 0, 0, 0 };

        QElapsedTimer timer;
        timer.start();
        sws_scale(
            slot->ctx,
            impl_->frame->data,
            impl_->frame->linesize,
            0,
            h,
            dstData,
            dstLinesize);
        const qint64 swsNs = timer.nsecsElapsed();
        swsScaleNs += swsNs;
        // Speculative reverse-cache fills are attributed separately so the
        // "cvt" readout reflects the frame actually being shown.
        if (isCacheFill) cacheConvertNs += swsNs;
        else convertNs += swsNs;

        // Shallow: shares the pooled buffer with the caller. The pool will not
        // reuse this entry again until every holder has released it.
        image = *target;

        perfStats_.fullFrameCopiesPerFrame = 1;
        // Carry the actual conversion size: "half" stopped being the whole
        // story once previews are converted straight to the displayed size.
        perfStats_.dstPixelFormat = (dstW != w)
            ? QStringLiteral("RGB32/BGRA %1x%2").arg(dstW).arg(dstH)
            : QStringLiteral("RGB32/BGRA");
    };

    // Reposition the demuxer and codec at or before `target`. Factored out
    // because the failure path needs it too: a decode that fails without having
    // seeked can be recovered by seeking and retrying, and duplicating the
    // flush-and-reset sequence is how the drain flags would drift apart.
    auto seekToFrame = [&](long long target) -> bool {
        const int64_t relTs = av_rescale_q(target, frameTb, impl_->streamTimeBase);
        const int64_t targetTs = impl_->streamStartTs + relTs;

        QElapsedTimer seekTimer;
        seekTimer.start();
        if (av_seek_frame(impl_->fmt, impl_->streamIndex, targetTs, AVSEEK_FLAG_BACKWARD) < 0) {
            return false;
        }
        avcodec_flush_buffers(impl_->codec);
        // Flushing refills the codec from the new position, so the stream is
        // no longer at EOF and the drain must be able to run again.
        impl_->inputEofReached = false;
        impl_->drainPacketSent = false;
        impl_->decoderFullyDrained = false;
        const double seekMs = static_cast<double>(seekTimer.nsecsElapsed()) / 1'000'000.0;
        perfStats_.lastSeekMs = seekMs;
        ++perfStats_.seekSamples;
        const double seekN = static_cast<double>(perfStats_.seekSamples);
        perfStats_.avgSeekMs += (seekMs - perfStats_.avgSeekMs) / seekN;
        // The landed frame's identity is resolved from its PTS by the caller's
        // walk; until then the decoder has no known position.
        impl_->lastDecodedFrame = -1;
        impl_->seekResolvePending = true;
        impl_->seekTargetFrame = target;
        return true;
    };

    // Decode linearly until the target frame is produced. Exactly one output
    // frame is converted per request (plus near-target frames cached for
    // reverse stepping). No forward fill: steady per-tick cost avoids the
    // burst stutter the old queue caused on heavy codecs (4K ProRes).
    auto decodeUntilTarget = [&](long long target) -> bool {
        while (true) {
            // Nothing left in the codec: the caller asked for a frame past the
            // real end of the stream.
            if (impl_->decoderFullyDrained) return false;

            if (!impl_->drainPacketSent) {
                QElapsedTimer readSendTimer;
                readSendTimer.start();
                const int readRes = av_read_frame(impl_->fmt, impl_->pkt);
                decodeNs += readSendTimer.nsecsElapsed();

                if (readRes < 0) {
                    // Input exhausted, but a frame-threaded decoder still holds
                    // up to thread_count frames in flight. Without this flush
                    // they are never emitted: the tail of every long-GOP file
                    // went missing and each request past the last emitted frame
                    // re-seeked and re-showed a stale image.
                    QElapsedTimer drainTimer;
                    drainTimer.start();
                    avcodec_send_packet(impl_->codec, nullptr);
                    decodeNs += drainTimer.nsecsElapsed();
                    impl_->inputEofReached = true;
                    impl_->drainPacketSent = true;
                    ++perfStats_.drainPacketsSent;
                } else if (impl_->pkt->stream_index != impl_->streamIndex) {
                    av_packet_unref(impl_->pkt);
                    continue;
                } else {
                    QElapsedTimer sendTimer;
                    sendTimer.start();
                    const int sendRes = avcodec_send_packet(impl_->codec, impl_->pkt);
                    decodeNs += sendTimer.nsecsElapsed();
                    av_packet_unref(impl_->pkt);
                    if (sendRes < 0) continue;
                }
            }

            while (true) {
                QElapsedTimer recvTimer;
                recvTimer.start();
                const int recvRes = avcodec_receive_frame(impl_->codec, impl_->frame);
                decodeNs += recvTimer.nsecsElapsed();
                if (recvRes == AVERROR_EOF) {
                    // Codec is empty and will produce nothing further until it
                    // is flushed by a seek or reopened.
                    impl_->decoderFullyDrained = true;
                    break;
                }
                // EAGAIN: needs more input. During drain no more input exists,
                // so the outer loop's drained check ends it rather than
                // spinning.
                if (recvRes != 0) break;
                if (impl_->drainPacketSent) ++perfStats_.drainFramesRecovered;

                const int64_t pts = (impl_->frame->best_effort_timestamp != AV_NOPTS_VALUE)
                                        ? impl_->frame->best_effort_timestamp
                                        : impl_->frame->pts;
                long long decodedFrame;
                if (impl_->seekResolvePending) {
                    // First frame after a frame-exact seek: trust the stream
                    // timestamp so the keyframe keeps its true index and the
                    // loop decodes forward to the exact target. Files without
                    // PTS fall back to the old label-as-target behavior.
                    impl_->seekResolvePending = false;
                    decodedFrame = (pts != AV_NOPTS_VALUE) ? frameFromPts(pts)
                                                           : impl_->seekTargetFrame;
                    // A backward seek should never land past the target; if a
                    // broken index puts us there, clamp so the UI stays put.
                    if (decodedFrame > impl_->seekTargetFrame) decodedFrame = impl_->seekTargetFrame;
                    if (decodedFrame < 0) decodedFrame = 0;
                } else {
                    decodedFrame = frameFromPts(pts);
                    if (decodedFrame < 0) decodedFrame = 0;
                    if (decodedFrame <= impl_->lastDecodedFrame) decodedFrame = impl_->lastDecodedFrame + 1;
                }
                impl_->prevDecodedPts = impl_->lastDecodedPts;
                impl_->lastDecodedPts = pts;
                impl_->lastDecodedFrame = decodedFrame;

                if (decodedFrame < target) {
                    ++walkFrames;
                    // Cache frames near the target for reverse stepping —
                    // but never during Scrub: previews are half-res and the
                    // extra conversions would drag scrub latency down.
                    // Walked frames are decoded regardless; converting one is
                    // the only extra cost, and it buys a future exact hit
                    // anywhere in this GOP. Cache fills are always converted
                    // at full resolution (fastOverride, not the Scrub
                    // half-res path), so an entry is safe to reuse later for
                    // stepping or for the exact landing frame.
                    const long long deltaToTarget = target - decodedFrame;
                    if (deltaToTarget <= cacheWindow) {
                        QImage cached;
                        convertCurrentFrame(cached, /*fastOverride=*/true, /*isCacheFill=*/true);
                        pushReverseCache(decodedFrame, cached, producesPreviewRes);
                        ++walkCacheConverts;
                    }

                    continue;
                }

                // decodedFrame >= target: convert and return it. Frames are
                // emitted in presentation order, so an overshoot means the
                // exact target does not exist in the stream; showing the
                // first frame at/after the target keeps ordering stable.
                convertCurrentFrame(outImage);
                currentFrame_ = decodedFrame;
                perfStats_.previewApproximate = false;
                perfStats_.previewTargetFrame = target;
                perfStats_.previewDisplayedFrame = decodedFrame;
                // Tagged rather than withheld: a soft entry is exactly what the
                // next backward drag step wants and exactly what a paused step
                // must not be given, and tryReverseCache enforces that. At or
                // below 1920px nothing is halved, so the entry is full-res and
                // serves everything.
                pushReverseCache(decodedFrame, outImage, producesPreviewRes);
                return true;
            }

            // Draining and the codec yielded nothing this pass: there is no
            // further input to feed it, so stop rather than spin on EAGAIN.
            if (impl_->drainPacketSent && !impl_->decoderFullyDrained) {
                impl_->decoderFullyDrained = true;
            }
        }
        // The requested frame was not produced. Returning true here (because
        // outImage still held the *previous* frame) is what made the viewer
        // repeat the tail frame and re-seek once per tick past EOF.
        ++perfStats_.staleSuccessPrevented;
        return false;
    };

    ++impl_->requestCounter;
    const long long prevRequestedFrame = impl_->prevRequestedFrame;
    impl_->prevRequestedFrame = frameIndex;
    const bool requestIsBackward = currentFrame_ >= 0 && frameIndex < currentFrame_;
    const bool requestIsSequentialForward = currentFrame_ >= 0 && frameIndex == currentFrame_ + 1;

    // Random-access requests (scrub drag, slider release, jump-steps) consult
    // the cache in either direction. It used to be checked only when the
    // request moved backward, so a forward scrub onto a frame decoded moments
    // earlier still paid a seek plus a full GOP walk. Sequential playback is
    // deliberately excluded: it must keep advancing the decoder rather than
    // being served from behind.
    const bool randomAccess = (mode != RequestMode::Playback) && !requestIsSequentialForward;
    if ((requestIsBackward || randomAccess) && tryReverseCache(frameIndex)) {
        updatePerfStats();
        return true;
    }

    // Audio-clocked playback corrects drift by advancing a frame or two extra,
    // and a seek for that is strictly the wrong trade: it costs a keyframe
    // landing plus a GOP walk (up to ~60ms on long-GOP H.264) to avoid decoding
    // two frames forward. Inside this window, walk. Playback only -- Step and
    // Scrub keep their existing seek behaviour.
    constexpr long long kPlaybackForwardWalkLimit = 4;
    const bool smallPlaybackForwardJump =
        mode == RequestMode::Playback &&
        currentFrame_ >= 0 &&
        frameIndex > currentFrame_ &&
        (frameIndex - currentFrame_) <= kPlaybackForwardWalkLimit;

    // Scrub no longer forces a seek. It used to, unconditionally, which meant
    // every mid-drag update landed on the keyframe at-or-before the target and
    // never walked forward to the real frame -- so dragging showed one new
    // picture per GOP (measured: frames 49..55 all displayed keyframe 30) while
    // the HUD reported `exact | delta 0`. Dragging is now a shuttle: the caller
    // walks the decoder forward one frame at a time and every frame is
    // presented, which is only affordable because forward decode is cheap and
    // it was the seek that was expensive. Genuine jumps -- backward moves, or a
    // forward gap larger than the caller's walk budget -- still seek via the
    // conditions below.
    // Can the decoder reach this frame by simply continuing forward? Only if it
    // has a known position and the target is still ahead of that position.
    //
    // `currentFrame_` is NOT a safe proxy for where the decoder is. A cache hit
    // sets it without advancing the decoder at all, so after a backward drag
    // served from cache the two diverge by however far the drag ran -- and the
    // next forward request looked "sequential" while the decoder was still
    // parked at the tail of the file with its drain packet sent. decodeUntilTarget
    // then had nothing left to give and the drag died with "No decodable frame
    // at target position". That was reachable before, but rare: it needs a run
    // of backward cache hits, and the hit rate at 4K used to be near zero.
    // Raising it to ~88% made a fast scrub with direction changes hit this
    // within a couple of gestures.
    const bool decoderCanReachByWalking =
        impl_->lastDecodedFrame >= 0 && frameIndex > impl_->lastDecodedFrame;

    const bool needSeek =
        (currentFrame_ < 0) ||
        !decoderCanReachByWalking ||
        (frameIndex < currentFrame_) ||
        (mode == RequestMode::Step && std::llabs(frameIndex - currentFrame_) > 1) ||
        // Re-request of the currently shown frame outside playback: seek so
        // the frame is re-decoded (e.g. full-res Step after a half-res
        // scrub preview of the same frame) instead of decoding forward.
        (mode != RequestMode::Playback && frameIndex == currentFrame_) ||
        (!requestIsSequentialForward && !smallPlaybackForwardJump
             && frameIndex > currentFrame_ + 1);

    if (needSeek) {
        didSeek = true;
#ifdef TRACE_WITH_FFMPEG
        // Measurement only (TRACE_SEEK_LOG=1): record which condition in the
        // needSeek expression above actually fired, evaluated in the same
        // order, plus the state the decision was made from.
        if (seekLogEnabled()) {
            const char* reason =
                (currentFrame_ < 0) ? "InitialOpen" :
                !decoderCanReachByWalking ? "DecoderBehindOrAtTarget" :
                (frameIndex < currentFrame_) ? "BackwardRequest_CacheMiss" :
                (mode == RequestMode::Step && std::llabs(frameIndex - currentFrame_) > 1) ? "StepJump" :
                (mode != RequestMode::Playback && frameIndex == currentFrame_) ? "RerequestOutsidePlayback" :
                (!requestIsSequentialForward && frameIndex > currentFrame_ + 1) ? "NonSequentialForwardGap" :
                "Unknown";
            const char* modeName = (mode == RequestMode::Playback) ? "Playback"
                                 : (mode == RequestMode::Scrub) ? "Scrub" : "Step";
            seekLogLine(QString("seek#%1 req#%2 reason=%3 mode=%4 requested=%5 prevRequested=%6 current=%7 lastDecoded=%8 dCurrent=%9 dLastDecoded=%10 pts=%11 prevPts=%12 seqFwd=%13 bframes=%14 codec=%15")
                .arg(impl_->seekCounter)
                .arg(impl_->requestCounter)
                .arg(reason)
                .arg(modeName)
                .arg(frameIndex)
                .arg(prevRequestedFrame)
                .arg(currentFrame_)
                .arg(impl_->lastDecodedFrame)
                .arg(frameIndex - currentFrame_)
                .arg(frameIndex - impl_->lastDecodedFrame)
                .arg(static_cast<long long>(impl_->lastDecodedPts))
                .arg(static_cast<long long>(impl_->prevDecodedPts))
                .arg(requestIsSequentialForward ? 1 : 0)
                .arg(impl_->codec ? impl_->codec->has_b_frames : -1)
                .arg(metadata_.codecName));
        }
        ++impl_->seekCounter;
#endif
        if (!seekToFrame(frameIndex)) {
            error = "Seek failed";
            return false;
        }

        // Every seek is frame-exact, Scrub included. Resolve the first decoded
        // frame from its PTS and decode forward to the true target. On
        // all-intra codecs (ProRes) the seek lands on the target anyway, so
        // this costs nothing there; on long-GOP H.264 it decodes up to a GOP
        // of frames — the price of showing the actual frame.
        //
        // Scrub used to take a shortcut here: it labelled the landed keyframe
        // as the requested frame and displayed that. It was instant and it was
        // wrong — up to a full GOP behind the pointer, with `delta 0` reported
        // for a frame that was nothing of the sort. A review tool cannot show
        // one frame and name it another, and telemetry that asserts its own
        // correctness is how it survived unnoticed. Dragging gets its frames
        // from the caller's forward walk now, not from mislabelled seeks.
        // (seekToFrame arms the resolve; this note is why it does.)
    }

    // How many frames to convert and cache on the way to the target.
    //
    // Without a seek this is ordinary forward decoding: fill the whole cache,
    // the frames are being decoded anyway.
    //
    // After a seek the conversions are latency the user waits on before their
    // frame appears — but they also prevent the *next* backward step from
    // seeking and walking the GOP again. Which cost dominates depends entirely
    // on conversion price: at 0.7ms a frame (1080p) caching a lot is nearly
    // free and saves whole seeks; at 14ms (4K) each entry is real delay.
    // So spend a fixed time budget rather than a fixed frame count.
    if (!didSeek) {
        cacheWindow = entriesThatFit(producesPreviewRes);
    } else if (impl_->seekWalkCacheWindowOverride >= 0) {
        cacheWindow = impl_->seekWalkCacheWindowOverride;
    } else {
        // Who is waiting decides how much to buy.
        //
        // A Step or Playback seek is a landing: one frame is wanted, now, and
        // every speculative conversion is delay in front of it. Buy few.
        //
        // A Scrub seek happens mid-drag, and the frames walked past are
        // precisely the ones the drag is about to ask for -- a backward drag
        // asks for them in reverse, one per slice. There the conversions are
        // not latency in front of one frame, they are the next dozen frames,
        // and declining them means paying the whole seek and GOP walk again for
        // each. Measured on 4K H.264 backward at the 18ms landing budget: one
        // or two frames cached per walk, 57% hits, 31ms a frame. Buy many.
        const double budgetMs = (mode == RequestMode::Scrub)
            ? impl_->scrubWalkCacheBudgetMs
            : 18.0;
        const double convertMs = perfStats_.avgConvertMs > 0.0 ? perfStats_.avgConvertMs : 4.0;
        const int affordable = static_cast<int>(budgetMs / convertMs);
        // Bound by what the budget holds at the size THIS request stores, not
        // at the full-res worst case: capping a half-res fill at the full-res
        // count would throw away three quarters of the budget.
        cacheWindow = std::clamp(affordable, 1, entriesThatFit(producesPreviewRes));
    }

    if (!decodeUntilTarget(frameIndex)) {
        // Failing without having seeked means the decoder was not where the
        // walk assumed. The condition above should prevent that, but a decode
        // failure is user-visible and a seek is cheap next to being wrong, so
        // recover rather than report: reposition and decode the frame properly.
        // A failure *after* a seek is honest -- the frame genuinely is not in
        // the stream -- and still returns false rather than a stale image.
        if (!didSeek && seekToFrame(frameIndex)) {
            ++perfStats_.recoveredDecodeFailures;
            if (decodeUntilTarget(frameIndex)) {
                updatePerfStats();
                error.clear();
                return true;
            }
        }
        error = "No decodable frame at target position";
        return false;
    }

    if (perfStats_.firstFrameMs <= 0.0 && perfStats_.samples == 0) {
        perfStats_.firstFrameMs = static_cast<double>(decodeNs + convertNs) / 1'000'000.0;
    }

    updatePerfStats();
    error.clear();
    return true;
#else
    Q_UNUSED(frameIndex);
    Q_UNUSED(outImage);
    Q_UNUSED(mode);
    error = "FFmpeg support not enabled at build time.";
    return false;
#endif
}

} // namespace trace::core
