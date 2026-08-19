#include "core/VideoDecoderFFmpeg.h"

#ifdef TRACE_WITH_FFMPEG
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/codec_desc.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>
#include <libavutil/opt.h>
// Rotation metadata (spec phase 12): the display matrix is a 3x3 affine in
// stream side data, and av_display_rotation_get is the only thing that should
// ever interpret it.
#include <libavutil/display.h>
// av_cpu_count(): the decode thread default is the machine's logical CPU count
// on intra-only sources, not FFmpeg's automatic count, which caps at 16.
#include <libavutil/cpu.h>
}
#endif

#include <cmath>
#include <algorithm>
#include <deque>

#include <QElapsedTimer>
#include <QByteArray>
#include <QFile>
#include <QDir>

#include "core/TimeFormat.h"

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

// Can this decoded format be handed to the GPU as three planes, and if so as
// what? Answered from the format DESCRIPTOR rather than a list of AVPixelFormat
// enumerators, so every yuv*p*le spelling FFmpeg has -- and any it gains -- is
// covered by the same four questions.
//
// Alpha is allowed here and then ignored: the caller passes the alpha-stripped
// format, and even if it did not, the shader samples three planes. That is the
// same trade alphaStrippedFormat() makes, for the same reason.
bool planarLayoutFor(AVPixelFormat fmt, PixelLayout* layout, int* bitDepth) {
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    if (!d) return false;
    if (!(d->flags & AV_PIX_FMT_FLAG_PLANAR)) return false;
    if (d->flags & (AV_PIX_FMT_FLAG_RGB | AV_PIX_FMT_FLAG_PAL | AV_PIX_FMT_FLAG_BITSTREAM)) return false;
    // Big-endian would need a byte swap on the way in, which is a repacking
    // pass and therefore exactly what this path exists to avoid. No format
    // Trace opens on Windows is BE.
    if (d->flags & AV_PIX_FMT_FLAG_BE) return false;
    if (d->nb_components < 3) return false;

    const int depth = d->comp[0].depth;
    if (depth != 8 && depth != 10 && depth != 12 && depth != 16) return false;
    // Every component must share the depth and sit at the bottom of its word;
    // anything else is a packed-in-planes layout that a straight memcpy would
    // misread.
    for (int i = 0; i < 3; ++i) {
        if (d->comp[i].depth != depth) return false;
        if (d->comp[i].shift != 0) return false;
        if (d->comp[i].offset != 0) return false;
        if (d->comp[i].plane != i) return false;
        if (d->comp[i].step != (depth > 8 ? 2 : 1)) return false;
    }

    if (d->log2_chroma_w == 1 && d->log2_chroma_h == 1)      *layout = PixelLayout::YUV420P;
    else if (d->log2_chroma_w == 1 && d->log2_chroma_h == 0) *layout = PixelLayout::YUV422P;
    else if (d->log2_chroma_w == 0 && d->log2_chroma_h == 0) *layout = PixelLayout::YUV444P;
    else return false;

    *bitDepth = depth;
    return true;
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

// The same choice expressed for the frame rather than for swscale. It travels
// with the pixels because the GPU path needs it as shader constants, where
// swscale is not the thing doing the conversion.
ColorInfo colorInfoFor(int coefficients, bool fullRange, bool inferred) {
    ColorInfo info;
    switch (coefficients) {
        case SWS_CS_ITU709:     info.matrix = ColorInfo::Matrix::BT709; break;
        case SWS_CS_ITU601:     info.matrix = ColorInfo::Matrix::BT601; break;
        case SWS_CS_BT2020:     info.matrix = ColorInfo::Matrix::BT2020; break;
        case SWS_CS_FCC:        info.matrix = ColorInfo::Matrix::Fcc; break;
        case SWS_CS_SMPTE240M:  info.matrix = ColorInfo::Matrix::Smpte240m; break;
        default:                info.matrix = ColorInfo::Matrix::Unspecified; break;
    }
    info.fullRange = fullRange;
    info.inferred = inferred;
    return info;
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

    // Threading mode chosen at open. `thread_type` can only be set before
    // avcodec_open2, so this cannot change without reopening the codec -- which
    // was built, measured and taken back out; see kPlaybackForwardWalkLimit.
    bool threadTypeIsFrame = false;

    // Frame-exact seek resolution (long-GOP codecs like H.264): after a
    // seek, the first decoded frame's index is resolved from its PTS instead
    // of being labeled as the requested frame, so decodeUntilTarget keeps
    // decoding forward from the keyframe to the *true* target frame.
    bool seekResolvePending = false;
    long long seekTargetFrame = -1;

    // Consulted once per packet inside the walk. Null means nothing can ask
    // for a walk to stop, which is the state the whole synchronous path runs
    // in and the reason installing this changes no behaviour by itself.
    VideoDecoderFFmpeg::CancelPredicate cancelPredicate;

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

    // Eviction is FIFO. LRU (promote-on-hit) was prototyped and measured: on
    // 4K H.264, where the cache actually fills and evicts, it produced an
    // identical hit rate (9/15 either way, 9 promotions) because the scrub
    // working set exceeds capacity rather than a hot subset being evicted.
    // At 1080p nothing is evicted at all, so the policy cannot matter there.
    //
    // A VideoFrame carries its own index and previewRes tag, so the entry type
    // is the frame itself; holding one is a refcount on its buffer.
    std::deque<VideoFrame> reverseCache;

    // Conversion targets are recycled so the pages stay committed and warm.
    //
    // A buffer is free when the pool is its only holder -- use_count() == 1.
    // This replaces the old QImage::isDetached() test, and with it the reason
    // the test existed: QImage::bits() detaches, so writing into a pooled
    // QImage the viewer or the cache still referenced deep-copied ~38MB at 4K,
    // every frame, of data sws_scale was about to overwrite in full. sws now
    // writes to buffer->data(), which cannot detach, so that cost is gone by
    // construction rather than avoided by picking the right entry.
    std::deque<std::shared_ptr<FrameBuffer>> convertPool;
    int convertPoolLimit = 16;

    // One pool for both kinds of buffer, because one session holds both: with
    // planar output on, full-resolution frames are planar while scrub previews
    // stay BGRA, and they alternate frame by frame through a drag.
    //
    // EVICTION ONLY WHEN THE POOL IS FULL, and that is the whole reason this is
    // one function. Evicting every non-matching unreferenced entry on each
    // acquire -- which is what the single-kind version did, harmlessly, when
    // BGRA was the only kind -- makes an alternating workload throw away the
    // other kind's buffers on every frame and reallocate them on the next. On
    // 4K ProRes 4444 that is a ~56MB planar allocation per landing, and it
    // measured as the drag shuttle going 7.8 -> 18.2ms/frame while every
    // per-frame cost stayed the same. Waiting until the pool is actually full
    // still stops a resolution change pinning it at stale sizes; it just takes
    // one pool's worth of frames to notice instead of one.
    template <typename Match, typename Make>
    std::shared_ptr<FrameBuffer> acquirePooled(Match matches, Make make) {
        for (auto& buf : convertPool) {
            if (buf.use_count() == 1 && matches(*buf)) return buf;
        }
        if (static_cast<int>(convertPool.size()) >= convertPoolLimit) {
            for (auto it = convertPool.begin(); it != convertPool.end();) {
                const bool stale = it->use_count() == 1 && !matches(**it);
                it = stale ? convertPool.erase(it) : it + 1;
            }
            if (static_cast<int>(convertPool.size()) >= convertPoolLimit) return nullptr;
        }
        auto buffer = make();
        if (!buffer) return nullptr;
        convertPool.push_back(buffer);
        return buffer;
    }

    std::shared_ptr<FrameBuffer> acquireConvertTarget(int w, int h, PixelLayout layout) {
        return acquirePooled(
            [&](const FrameBuffer& b) {
                return b.width() == w && b.height() == h && b.layout() == layout;
            },
            [&] { return FrameBuffer::allocate(w, h, layout); });
    }

    std::shared_ptr<FrameBuffer> acquirePlanarTarget(int w, int h, PixelLayout layout,
                                                     int bitDepth) {
        return acquirePooled(
            [&](const FrameBuffer& b) {
                return b.width() == w && b.height() == h && b.layout() == layout
                       && b.bitDepth() == bitDepth;
            },
            [&] { return FrameBuffer::allocatePlanar(w, h, layout, bitDepth); });
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
    // Footprint of ONE full-resolution cache entry, in the layout actually being
    // stored.
    //
    // This exists because the figure it replaces was `w * h * 4` -- the BGRA
    // footprint -- and has been wrong since GATE C made full-resolution entries
    // planar (`e8566a4`). At 4K it priced an 11.86MB yuv420p plane set at
    // 33.2MB, so the derived entry count read 11 where the byte budget really
    // holds 32, and that count is what clamps how far a seek walk may fill. The
    // fault was invisible because nothing enforces the count -- eviction is by
    // bytes -- so the cache filled correctly while every decision *derived* from
    // the count was made on a third of the truth.
    //
    // Seeded at open with the BGRA worst case, because something has to be
    // available before any frame exists, and then REPLACED by the size of the
    // first full-resolution frame actually stored. Observing it rather than
    // predicting it is deliberate: a predicted size has to be kept in agreement
    // with the converter forever, and that agreement is exactly what lapsed. The
    // same reasoning made `previewRes` a property set by the conversion rather
    // than by the call site (`03d840e`).
    long long fullResEntryBytes = 0;
    // -1 = adapt per request from measured conversion cost (see below).
    // Overridden by TRACE_SEEK_CACHE_WINDOW for A/B.
    int seekWalkCacheWindowOverride = -1;
    // How much conversion a *drag* may spend filling the cache on one seek
    // walk, against 18ms for a Step landing. Higher fills more of the GOP per
    // seek: fewer seeks, more hits, at the cost of a longer pause on the frame
    // that pays for it. TRACE_SCRUB_FILL_MS to A/B.
    //
    // Raised 60 -> 240 once the walk moved off the UI thread (step 5). The old
    // value was chosen when this ran synchronously, where a 240ms fill was a
    // 240ms frozen window; on the worker it costs the UI thread nothing, and
    // the measured `ui gap` is unchanged between the two. Measured on 4K H.264
    // backward, which is the case that pays for seeks:
    //
    //   fill  30ms   hit 70.2%  dec  46.0 f/s  seeks 12  stalls 9 of 37
    //   fill  60ms   hit 86.4%  dec  91.5 f/s  seeks 10  stalls 7 of 73
    //   fill 120ms   hit 89.1%  dec  92.0 f/s  seeks  8  stalls 6 of 78
    //   fill 240ms   hit 91.9%  dec 121.3 f/s  seeks  7  stalls 5 of 97
    //   fill 480ms   hit 92.9%  dec 124.0 f/s  seeks  6  stalls 4 of 98
    //
    // Flat past 240, so that is where it sits. Stalls and seeks both fall, so
    // the longer fill is not being paid for in the place it would show. Memory
    // is unaffected: eviction is by bytes against the same 192MB budget, and
    // occupancy measured 184.6MB at 240 against 190.7MB at 60.
    //
    // 1080p gains far less (hit 90.9 -> 91.8%) because its cache is bound by
    // bytes rather than by this budget -- nothing is halved at 1080p, so 24
    // full-res entries fill the budget. The win is where previews are small
    // enough for the cache to hold a GOP's worth of them.
    double scrubWalkCacheBudgetMs = 240.0;
    // Size the viewer draws at, or 0 when unknown. Used only as a *ceiling* on
    // scrub preview conversion, never as a floor: a preview is never made
    // larger than half resolution, and never larger than it will be shown.
    int previewDisplayW = 0;
    int previewDisplayH = 0;
#endif
};

int VideoDecoderFFmpeg::cpuCount() {
#ifdef TRACE_WITH_FFMPEG
    return av_cpu_count();
#else
    return 0;
#endif
}

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

int VideoDecoderFFmpeg::setScrubPreviewSize(QSize size) {
#ifdef TRACE_WITH_FFMPEG
    if (!impl_) return 0;
    // TRACE_PREVIEW_DISPLAY_SIZE=0 pins previews back to the plain half-res
    // rule, so the display-size conversion can be A/B'd on feel and sharpness
    // against the behaviour it replaced without a rebuild.
    static const bool useDisplaySize = envInt("TRACE_PREVIEW_DISPLAY_SIZE", 1) != 0;
    const int w = useDisplaySize ? std::max(0, size.width()) : 0;
    const int h = useDisplaySize ? std::max(0, size.height()) : 0;
    if (w == impl_->previewDisplayW && h == impl_->previewDisplayH) return 0;
    impl_->previewDisplayW = w;
    impl_->previewDisplayH = h;
    // Preview entries were converted at the old size. They would still draw --
    // the viewer scales whatever it is given -- but a drag would then run
    // through frames of visibly different sharpness, which reads as the picture
    // breaking up rather than as a resize. Drop them and let the drag refill.
    const int discarded = static_cast<int>(impl_->reverseCache.size());
    impl_->reverseCache.clear();
    impl_->reverseCacheBytes = 0;
    // The occupancy the HUD prints is otherwise only refreshed inside frameAt,
    // so after a clear it would go on reporting a cache that no longer exists
    // until the next decode -- and a resize is exactly the gesture that clears
    // without decoding. That is a stale instrument of the kind this project has
    // now been caught by four times, and it would have read as "the resize cost
    // nothing" on the one measurement built to find out.
    perfStats_.cacheOccupancy = 0;
    perfStats_.cacheBytes = 0;
    return discarded;
#else
    Q_UNUSED(size);
    return 0;
#endif
}

void VideoDecoderFFmpeg::setPlanarOutputEnabled(bool enabled) {
#ifdef TRACE_WITH_FFMPEG
    if (enabled == planarOutput_) return;
    planarOutput_ = enabled;
    // Cached frames carry the layout that was in force when they were made, and
    // a renderer that has just changed cannot present the other kind. Same
    // reasoning as setScrubPreviewSize: an entry describes a decision, so the
    // entries do not survive the decision changing.
    if (impl_) {
        impl_->reverseCache.clear();
        impl_->reverseCacheBytes = 0;
    }
#else
    Q_UNUSED(enabled);
#endif
}

// An RGB copy of a frame that has already been decoded (spec phase 14).
//
// See the header for why this has to exist at all. Two things about the
// implementation are deliberate:
//
// ITS OWN CONTEXT, CREATED AND DESTROYED HERE. The four-slot LRU is sized and
// keyed for the three configurations a scrub cycle alternates between, and
// letting a once-per-keypress conversion evict one would make the next playback
// frame pay a rebuild -- the ~8-9ms cost `5e57d86` was written to remove.
//
// THE FRAME'S OWN COLORIMETRY, not the decoder's current state. `frame.color`
// was resolved when the frame was converted; perfStats_ has been overwritten
// many times since. A copy taken with the wrong matrix looks plausible and is
// wrong, which is the worst kind of output for a review tool.
bool VideoDecoderFFmpeg::frameToRgbImage(const VideoFrame& frame, QImage& out,
                                         QString& error) const {
    out = QImage();
#ifdef TRACE_WITH_FFMPEG
    if (frame.isNull() || !frame.buffer) {
        error = QStringLiteral("No frame to copy");
        return false;
    }
    const FrameBuffer& buffer = *frame.buffer;

    // Already RGB -- the CPU backend's path, and every still image. A deep copy
    // rather than the zero-copy view: the clipboard outlives this frame, and
    // the buffer underneath it goes back to the conversion pool.
    if (!isPlanarYuv(buffer.layout())) {
        const QImage view = frame.toQImage();
        if (view.isNull()) {
            error = QStringLiteral("Frame layout cannot be copied");
            return false;
        }
        out = view.copy();
        return !out.isNull();
    }

    AVPixelFormat srcFmt = AV_PIX_FMT_NONE;
    switch (buffer.layout()) {
        case PixelLayout::YUV420P:
            srcFmt = buffer.bitDepth() <= 8    ? AV_PIX_FMT_YUV420P
                     : buffer.bitDepth() <= 10 ? AV_PIX_FMT_YUV420P10LE
                     : buffer.bitDepth() <= 12 ? AV_PIX_FMT_YUV420P12LE
                                               : AV_PIX_FMT_YUV420P16LE;
            break;
        case PixelLayout::YUV422P:
            srcFmt = buffer.bitDepth() <= 8    ? AV_PIX_FMT_YUV422P
                     : buffer.bitDepth() <= 10 ? AV_PIX_FMT_YUV422P10LE
                     : buffer.bitDepth() <= 12 ? AV_PIX_FMT_YUV422P12LE
                                               : AV_PIX_FMT_YUV422P16LE;
            break;
        case PixelLayout::YUV444P:
            srcFmt = buffer.bitDepth() <= 8    ? AV_PIX_FMT_YUV444P
                     : buffer.bitDepth() <= 10 ? AV_PIX_FMT_YUV444P10LE
                     : buffer.bitDepth() <= 12 ? AV_PIX_FMT_YUV444P12LE
                                               : AV_PIX_FMT_YUV444P16LE;
            break;
        default: break;
    }
    if (srcFmt == AV_PIX_FMT_NONE) {
        error = QStringLiteral("Frame layout cannot be copied");
        return false;
    }

    const int w = buffer.width();
    const int h = buffer.height();
    QImage image(w, h, QImage::Format_RGB32);
    if (image.isNull()) {
        error = QStringLiteral("Out of memory copying frame");
        return false;
    }

    // Accurate, not fast. This is a still being taken away for inspection, so it
    // is the Step case rather than the Playback one -- the same reasoning that
    // gives a paused frame SWS_FULL_CHR_H_INT|SWS_ACCURATE_RND.
    SwsContext* ctx = createSwsContext(w, h, srcFmt, w, h, AV_PIX_FMT_BGRA,
                                       /*fast=*/false);
    if (!ctx) {
        error = QStringLiteral("Could not create the conversion context");
        return false;
    }

    int coefficients = SWS_CS_ITU709;
    switch (frame.color.matrix) {
        case ColorInfo::Matrix::BT601:      coefficients = SWS_CS_ITU601; break;
        case ColorInfo::Matrix::BT2020:     coefficients = SWS_CS_BT2020; break;
        case ColorInfo::Matrix::Fcc:        coefficients = SWS_CS_FCC; break;
        case ColorInfo::Matrix::Smpte240m:  coefficients = SWS_CS_SMPTE240M; break;
        case ColorInfo::Matrix::BT709:
        case ColorInfo::Matrix::Unspecified:
        default:                            coefficients = SWS_CS_ITU709; break;
    }
    applyColorspaceDetails(ctx, coefficients, frame.color.fullRange);

    const uint8_t* srcData[4] = {buffer.data(0), buffer.data(1), buffer.data(2), nullptr};
    const int srcStride[4] = {buffer.bytesPerLine(0), buffer.bytesPerLine(1),
                              buffer.bytesPerLine(2), 0};
    if (!srcData[0] || !srcData[1] || !srcData[2]) {
        sws_freeContext(ctx);
        error = QStringLiteral("Frame is missing a plane");
        return false;
    }

    uint8_t* dstData[4] = {image.bits(), nullptr, nullptr, nullptr};
    const int dstStride[4] = {static_cast<int>(image.bytesPerLine()), 0, 0, 0};
    const int rows = sws_scale(ctx, srcData, srcStride, 0, h, dstData, dstStride);
    sws_freeContext(ctx);

    if (rows != h) {
        error = QStringLiteral("Frame conversion failed");
        return false;
    }
    out = image;
    return true;
#else
    Q_UNUSED(frame);
    error = QStringLiteral("This build has no video support");
    return false;
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

void VideoDecoderFFmpeg::setCancelPredicate(CancelPredicate predicate) {
#ifdef TRACE_WITH_FFMPEG
    if (impl_) impl_->cancelPredicate = std::move(predicate);
#else
    Q_UNUSED(predicate);
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
    lastRequestAbandoned_ = false;
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
    // THREAD COUNT IS NOT FREE AND 0 IS NOT "ALL OF THEM". FFmpeg's automatic
    // count caps at 16 and warns above it, so on a 32-thread box half the machine
    // sits idle on a source that needs all of it. Swept on the FINAL minimal GCC
    // build, decode-only, 7680x4320 ProRes 4444 XQ, worst of two sustained passes:
    //
    //   t=16 19.05 fps | t=20 21.09 | t=24 22.68 | t=28 24.08
    //   t=32 25.24     | t=40 25.24 | t=48 25.45 | t=64 25.32
    //
    // It knees at the machine's logical CPU count and is flat beyond it, so the
    // default is that count rather than a literal 32 -- a four-core box must not
    // inherit this machine's answer.
    //
    // RAISED FOR INTRA-ONLY AND NOWHERE ELSE, because thread_count means two
    // different things. Under slice threading it is how many threads share ONE
    // frame, so more of them is simply more of the machine; under frame threading
    // it is how many frames are IN FLIGHT, and a deeper pipeline costs a longer
    // refill after every seek and flush -- which is the exact mechanism that makes
    // frame threading unusable for random access (see applyThreadTypeFor's
    // successor note at kPlaybackForwardWalkLimit). Long-GOP therefore keeps
    // FFmpeg's automatic count, and the discriminator is the same `intraOnly` the
    // rest of this file already trusts.
    //
    // Verified harmless where it now applies: 4K ProRes 4444 cadence 99.8% both
    // with and without, `0 of 260` both, while `dec` falls 13.00 -> 9.30ms.
    const int cpuThreads = std::min(av_cpu_count(), 64);
    impl_->codec->thread_count =
        envInt("TRACE_DECODE_THREADS", intraOnly ? cpuThreads : 0);
    // TRACE_LONGGOP_SLICE_THREADS=1 applies the intra-only rule to long-GOP too.
    // Measurement scaffolding, off by default.
    //
    // Reverse playback measured a fixed cost of ~30ms per seek that does NOT
    // scale with resolution -- 30.4ms at 4K and 30.2ms at 1080p, from a
    // two-point solve (docs/reverse-shuttle-plan.md section 5) -- against a seek
    // that itself measures 5.8-7.7ms. Something resolution-independent is
    // spending 20-24ms on every seek, and the pipeline refill described in the
    // comment above is the obvious candidate: it is the same mechanism, on the
    // same code path, that moved intra-only codecs to slice threading.
    //
    // Forward playback throughput is what this trades against and is the control
    // for any run with it set.
    const bool forceSliceThreads = envFlagSet("TRACE_LONGGOP_SLICE_THREADS");
    // TRACE_INTRA_FRAME_THREADS=1 is the SYMMETRIC control, and it did not exist
    // until the 8K ProRes report: TRACE_LONGGOP_SLICE_THREADS tests slice-only on
    // long-GOP, and nothing tested frame threading on intra-only.
    //
    // The premise of the intra-only rule above was measured in July 2026, on the
    // UI thread, before f77d472 moved random-access scrub decode onto a worker
    // under a lease -- so the ~100ms seek stall it protects against is no longer
    // on the thread that matters. What it costs is forward throughput, and that
    // cost scales with frame size, which is invisible at 1080p and 4K.
    //
    // A REFERENCE-DECODER BENCHMARK ANSWERS THIS QUESTION WRONG, AND IT ANSWERS
    // IT CONFIDENTLY. `ffmpeg -f null` on this box makes slice-only look FASTER
    // than frame+slice on every ProRes file in the set -- 4096x2304 78.2 vs 75.0
    // fps, 4448x3096 59.1 vs 50.8, 7680x4320 XQ 19.9 vs 17.5 -- which would have
    // closed this as refuted. In Trace the same switch takes the 8K XQ plate from
    // 33.8% to 58.0% of real time.
    //
    // The inversion is the whole point. `-f null` decodes and discards, so there
    // is no other work for a frame-threaded decoder to overlap WITH and frame
    // threading only adds its per-thread state copies. Trace's playback tick
    // decodes one frame and then converts, uploads and paints it on the same
    // thread; with frame threading the next frame decodes underneath all of that.
    // The evidence is the handler, not the rate: 89.7ms -> 43.3ms per frame.
    //
    // Measure the pipeline you ship, not the decoder in isolation.
    //
    // A PER-MODE SWITCH WAS BUILT AND TAKEN BACK OUT -- see the four-cell table
    // at kPlaybackForwardWalkLimit. Frame threading for playback and slice for
    // random access is the obvious reading of the trade, and it does not survive
    // the real-time frame drop: dropping makes playback ask for jumps, which on
    // intra-only is random access, so the mode that wants frame threading stops
    // existing exactly when the source is heavy enough to want it.
    const bool intraFrameThreads = envFlagSet("TRACE_INTRA_FRAME_THREADS");
    impl_->threadTypeIsFrame = !((intraOnly && !intraFrameThreads) || forceSliceThreads);
    impl_->codec->thread_type = impl_->threadTypeIsFrame
        ? (FF_THREAD_FRAME | FF_THREAD_SLICE)
        : FF_THREAD_SLICE;
    metadata_.intraOnly = intraOnly;

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
        // 384MB, raised from 192 in Aug 2026 because the old figure was the
        // binding constraint on drag hitches and nothing else was.
        //
        // The route here matters, because the obvious lever was the wrong one.
        // Plan section 15.5 item 1 nominated "convert Step and cache-fill
        // conversions to display size" as the way to multiply cache depth. GATE
        // C had already collected most of that without anyone noticing: a
        // full-res 1080p entry is a yuv420p PLANE SET at 3.11MB, not an 8.29MB
        // BGRA frame, so depth was already 64 entries rather than the 24 that
        // note assumed. What display-size conversion would add on top is 3.11MB
        // -> 2.54MB, eighteen percent, and it would pay for that by replacing a
        // 0.25ms plane copy with a multi-millisecond swscale resample on the
        // one path whose whole cost is the round trip. It is a losing trade now
        // even though it was a winning one when it was written.
        //
        // Raising the budget is what the measurement actually pointed at.
        // Reversal drags at win 1284x1067 on d3d11, 192 -> 384MB:
        //
        //   1080p H.264    hitch 8,8 -> 3,2   seeks 11 -> 4,3   hit 96.8 -> 98.9%
        //   4K H.264       hitch 3   -> 1,1   seeks  5 -> 3,3   hit 97.7 -> 98.3%
        //   ProRes 4444    hitch 7   -> 5     seeks 98 -> 97    hit  9.9 -> 19.5%
        //
        // 4444 moves least and that is structural, not a shortfall: every frame
        // is a keyframe, a seek lands on the target, no intermediate frames are
        // ever produced and there is nothing to cache (section 15.1). Do not
        // try to fix its hit rate with more bytes.
        //
        // Cost is memory and only memory: working set 396 -> 598MB on 1080p,
        // 677 -> 902MB on 4K H.264, 572 -> 710MB on 4444. 768MB was measured
        // too and is past the knee -- 1080p goes to hitch 1 and seeks 2 for
        // another ~400MB.
        //
        // The knob stays, as the control for any of the above and as the way to
        // buy more depth on a machine that can afford it. It is also how the
        // question was settled at all: quadrupling the budget is a strictly
        // stronger version of anything a cheaper entry representation could buy,
        // so hitches falling when it was raised is what proved they were misses.
        const int budgetMb = std::clamp(envInt("TRACE_REVERSE_CACHE_MB", 384), 16, 4096);
        impl_->reverseCacheBudgetBytes = static_cast<long long>(budgetMb) * 1024 * 1024;
        impl_->reverseCacheBytes = 0;
        // Worst case: every entry full resolution. Half-res scrub entries cost
        // a quarter of this, so the byte budget will hold about four times as
        // many of them -- which is the point.
        // Seed only. The first full-resolution frame stored replaces it with
        // what an entry really costs; see fullResEntryBytes.
        impl_->fullResEntryBytes = frameBytes;
        const int byFootprint = frameBytes > 0
            ? static_cast<int>(impl_->reverseCacheBudgetBytes / frameBytes)
            : 12;
        // Upper bound raised 32 -> 256 with the pricing fix. The old cap was set
        // when an entry was priced as BGRA and 32 was comfortably above anything
        // reachable; priced honestly, 4K planar already reaches 32 exactly and
        // 1080p reaches 123, so the cap had become a second wrong answer sitting
        // behind the first. 256 matches entriesThatFit's own bound.
        impl_->reverseCacheCapacity = std::clamp(byFootprint, 4, 256);
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
    // Clamped at the USE site, not here. Clamping at open pinned it to a count
    // derived from the seed footprint -- the BGRA one -- so asking for a
    // 30-frame fill at 4K silently got 11 and the experiment that asked for it
    // read as refuted. The bound belongs where the real entry size is known.
    impl_->seekWalkCacheWindowOverride = envInt("TRACE_SEEK_CACHE_WINDOW", -1);
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

    // THE SAME TWO FACTS, AS METADATA RATHER THAN AS TELEMETRY (spec phase 13),
    // and written here beside the two lines above so the difference is visible
    // in one place rather than argued about in two.
    //
    // The perfStats pair is rewritten by every conversion and measures the
    // PIXEL, not the component: yuv420p reads 12 there, and a 4444 file reads a
    // format with " (a-skip)" appended once playback drops its alpha plane.
    // Neither is a property of the file. These are: read once, from the
    // descriptor, and never touched again.
    if (srcDesc) {
        if (srcDesc->name) metadata_.pixelFormatName = QString::fromUtf8(srcDesc->name);
        // comp[0] is the luma/first component. Every format Trace opens has a
        // uniform depth across components; a hypothetical one that did not would
        // still report its first honestly rather than an average.
        metadata_.bitsPerComponent = srcDesc->comp[0].depth;
        metadata_.bitsPerPixel = av_get_bits_per_pixel(srcDesc);
    }

    AVRational fr = av_guess_frame_rate(impl_->fmt, stream, nullptr);
    if (fr.num <= 0 || fr.den <= 0) fr = AVRational{24, 1};
    impl_->fpsQ = fr;
    // Both, and the rational first: the double is derived from it, not the
    // other way round. av_q2d(24000/1001) is 23.976023976023978, which is the
    // nearest double and not the rate -- keeping the pair means a consumer that
    // needs exactness has it, without changing anything that reads `fps` today.
    metadata_.fpsNum = fr.num;
    metadata_.fpsDen = fr.den;
    metadata_.fps = av_q2d(fr);

    impl_->streamTimeBase = stream->time_base;
    impl_->streamStartTs = (stream->start_time != AV_NOPTS_VALUE) ? stream->start_time : 0;

    // WHAT THE FILE STATES, READ VERBATIM INCLUDING ITS ABSENCE (spec phase 13).
    //
    // FFmpeg's av_color_*_name() return nullptr for UNSPECIFIED, and that is
    // exactly the answer wanted: the field stays empty, the inspector prints
    // "Untagged", and nothing anywhere substitutes a plausible value. The
    // matrix here is the TAGGED one and may differ from the one playback used --
    // see the header for why they are kept apart.
    {
        const AVCodecParameters* p = stream->codecpar;
        if (p->color_primaries != AVCOL_PRI_UNSPECIFIED) {
            if (const char* n = av_color_primaries_name(p->color_primaries))
                metadata_.taggedColorPrimaries = QString::fromLatin1(n);
        }
        if (p->color_trc != AVCOL_TRC_UNSPECIFIED) {
            if (const char* n = av_color_transfer_name(p->color_trc))
                metadata_.taggedColorTransfer = QString::fromLatin1(n);
        }
        if (p->color_space != AVCOL_SPC_UNSPECIFIED) {
            if (const char* n = av_color_space_name(p->color_space))
                metadata_.taggedColorMatrix = QString::fromLatin1(n);
        }
        if (p->color_range != AVCOL_RANGE_UNSPECIFIED) {
            metadata_.hasTaggedRange = true;
            metadata_.taggedRangeIsFull = (p->color_range == AVCOL_RANGE_JPEG);
        }

        if (impl_->fmt->iformat) {
            if (impl_->fmt->iformat->name)
                metadata_.containerName = QString::fromUtf8(impl_->fmt->iformat->name);
            if (impl_->fmt->iformat->long_name)
                metadata_.containerLongName = QString::fromUtf8(impl_->fmt->iformat->long_name);
        }
        if (const char* prof = av_get_profile_name(impl_->codecDef, p->profile))
            metadata_.codecProfile = QString::fromLatin1(prof);
        metadata_.videoStreamIndex = stream->index;
        metadata_.videoTrackId = stream->id;
        metadata_.videoBitrateBps = p->bit_rate > 0 ? p->bit_rate : -1;

        // The audio stream is chosen the same way the audio path chooses it, so
        // the inspector reports the track that would actually play rather than
        // the first one in the file.
        const int aIdx = av_find_best_stream(impl_->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
        if (aIdx >= 0) {
            const AVStream* as = impl_->fmt->streams[aIdx];
            const AVCodecParameters* ap = as->codecpar;
            metadata_.hasAudio = true;
            if (const AVCodecDescriptor* d = avcodec_descriptor_get(ap->codec_id))
                metadata_.audioCodecName = QString::fromUtf8(d->name);
            if (const AVCodec* ac = avcodec_find_decoder(ap->codec_id)) {
                if (const char* prof = av_get_profile_name(ac, ap->profile))
                    metadata_.audioProfile = QString::fromLatin1(prof);
            }
            metadata_.audioSampleRate = ap->sample_rate;
            metadata_.audioChannels = ap->ch_layout.nb_channels;
            char layout[64] = {0};
            if (av_channel_layout_describe(&ap->ch_layout, layout, sizeof(layout)) > 0)
                metadata_.audioChannelLayout = QString::fromLatin1(layout);
            metadata_.audioBitrateBps = ap->bit_rate > 0 ? ap->bit_rate : -1;
            metadata_.audioStreamIndex = as->index;
            metadata_.audioTrackId = as->id;
        }
    }

    // SAMPLE ASPECT AND ROTATION, READ AND NEVER ASSUMED (spec phase 12).
    //
    // Both are read here, beside the timecode, because they are the same kind of
    // thing: a statement the container makes about how the media is meant to be
    // presented, which is nothing to do with the pixels that come out of the
    // decoder. Section 4's first requirement is that the window adopt the real
    // display ratio, and it says outright not to assume that is width/height.
    {
        // av_guess_sample_aspect_ratio, not codecpar->sample_aspect_ratio: it is
        // the function that composes the codec's SAR with the container's and
        // resolves which one wins. That composition IS the spec's "display
        // aspect ratio metadata when authoritative" -- a demuxer that reads a
        // container DAR turns it into a SAR here, so a second field asking the
        // same question a different way could only ever disagree with this one.
        const AVRational sar = av_guess_sample_aspect_ratio(impl_->fmt, stream, nullptr);
        if (sar.num > 0 && sar.den > 0) {
            metadata_.sarNum = sar.num;
            metadata_.sarDen = sar.den;
            // A file that states 1:1 and a file that states nothing both give
            // square pixels; only the first is evidence, and the HUD says which.
            metadata_.sarStated = true;
        }

        const uint8_t* matrix = nullptr;
#if LIBAVCODEC_VERSION_MAJOR >= 61
        // FFmpeg 7 moved stream side data onto codecpar. Guarded rather than
        // assumed: this box builds against 8.x, and CI resolves its own vcpkg
        // baseline, so the one place they could differ should not be a
        // compile error discovered on a runner.
        if (const AVPacketSideData* sd = av_packet_side_data_get(
                stream->codecpar->coded_side_data, stream->codecpar->nb_coded_side_data,
                AV_PKT_DATA_DISPLAYMATRIX)) {
            if (sd->size >= 9 * 4) matrix = sd->data;
        }
#else
        {
            size_t sz = 0;
            const uint8_t* sd = av_stream_get_side_data(stream, AV_PKT_DATA_DISPLAYMATRIX, &sz);
            if (sd && sz >= 9 * 4) matrix = sd;
        }
#endif
        if (matrix) {
            // av_display_rotation_get returns the ANTICLOCKWISE angle the
            // picture must be rotated by to be shown correctly, in degrees, and
            // it is the only thing that should ever read those nine integers.
            const double theta = av_display_rotation_get(reinterpret_cast<const int32_t*>(matrix));
            if (!std::isnan(theta)) {
                // To clockwise, then to the nearest quarter turn. The matrix is
                // a general affine and can legitimately hold 12.5 degrees; Trace
                // has no arbitrary-rotation path, so it is snapped and the
                // snapping is REPORTED rather than hidden -- silently squaring
                // off an arbitrary rotation looks exactly like having nothing to
                // rotate at all.
                double cw = -theta;
                while (cw < 0.0) cw += 360.0;
                while (cw >= 360.0) cw -= 360.0;
                const int quarter = static_cast<int>(std::lround(cw / 90.0)) % 4;
                metadata_.rotationDegrees = quarter * 90;
                metadata_.rotationSnapped = std::abs(cw - quarter * 90.0) > 0.5
                                         && std::abs(cw - 360.0) > 0.5;
            }
        }
    }

    // SOURCE TIMECODE, READ AND NEVER SYNTHESISED (spec phase 7).
    //
    // Three places are checked, in the order a real file is most likely to
    // carry it. FFmpeg's mov demuxer publishes a tmcd track's value on the
    // FORMAT dictionary; MXF and some MOVs put it on the video stream; and a
    // few containers leave it only on the data stream that carries it. Checking
    // all three is a handful of dictionary lookups at open time and is the
    // difference between "this file has no timecode" and "we looked in one
    // place".
    //
    // The value is then PARSED AND RE-FORMATTED rather than stored raw, so
    // anything unreadable becomes "no timecode" here instead of reaching a
    // readout that would print it verbatim and call it SMPTE.
    {
        const AVDictionaryEntry* tcEntry =
            av_dict_get(stream->metadata, "timecode", nullptr, 0);
        if (!tcEntry) tcEntry = av_dict_get(impl_->fmt->metadata, "timecode", nullptr, 0);
        if (!tcEntry) {
            for (unsigned i = 0; i < impl_->fmt->nb_streams && !tcEntry; ++i) {
                tcEntry = av_dict_get(impl_->fmt->streams[i]->metadata, "timecode", nullptr, 0);
            }
        }
        if (tcEntry && tcEntry->value) {
            TimeFormat::Timecode parsed;
            if (TimeFormat::parseTimecode(QString::fromUtf8(tcEntry->value), parsed)) {
                // A drop-frame flag at a rate that has no drop-frame is
                // dropped rather than honoured: the file is malformed, and
                // carrying the claim forward would make Go to Timecode do
                // arithmetic the rate cannot support.
                parsed.dropFrame = parsed.dropFrame
                                && TimeFormat::dropFrameApplies(metadata_.fpsNum, metadata_.fpsDen);
                // Frames must be a number this rate can actually produce.
                if (parsed.frames < TimeFormat::nominalRate(metadata_.fpsNum, metadata_.fpsDen)) {
                    metadata_.startTimecode = TimeFormat::formatTimecode(parsed);
                    metadata_.startTimecodeDropFrame = parsed.dropFrame;
                    metadata_.hasStartTimecode = true;
                }
            }
        }
    }

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
            "\tdur=%26\tframes=%27\tcolorspace=%28\trange=%29\ttimecode=%30"
            // Spec phase 12. `sar` carries "stated" explicitly rather than
            // leaving 1/1 to mean two different things, and `dar` is the
            // composed answer so a fixture can be checked against one number
            // instead of being recomputed by whoever reads the log.
            "\tsar=%31/%32%33\trot=%34%35\tdar=%36\tnatural=%37x%38"
            // Spec phase 13. `tag*` is what the FILE states, which is not what
            // `colorspace=`/`range=` above report -- those are the enums the
            // decoder then applies a heuristic to. An empty field here would be
            // ambiguous in a tab-separated log, so absence prints as the word.
            "\ttagpri=%39\ttagtrc=%40\ttagmtx=%41\ttagrange=%42"
            "\tcontainer=%43\tprofile=%44\tvtrack=%45\taudio=%46"
            // `pixfmt=` above is perfStats_'s copy and is what the last
            // CONVERSION saw; `encpixfmt=` is the file's own. They differ on
            // 4444 the moment the alpha plane is dropped. `depth=` is per
            // COMPONENT and `bpp=` per pixel -- printed together because the
            // pair is the whole point: 8 and 12 on yuv420p.
            "\tencpixfmt=%47\tdepth=%48\tbpp=%49")
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
            .arg(static_cast<int>(par->color_range))
            // "none" rather than an empty field: the whole point of this
            // extraction is that absence is a real answer, and a blank column
            // in a tab-separated log reads as a bug in the logger.
            .arg(metadata_.hasStartTimecode ? metadata_.startTimecode
                                            : QStringLiteral("none"))
            .arg(metadata_.sarNum).arg(metadata_.sarDen)
            .arg(metadata_.sarStated ? QString() : QStringLiteral("(assumed)"))
            .arg(metadata_.rotationDegrees)
            .arg(metadata_.rotationSnapped ? QStringLiteral("(snapped)") : QString())
            .arg(QString::number(metadata_.displayAspect(), 'f', 6))
            .arg(metadata_.naturalDisplaySize().width())
            .arg(metadata_.naturalDisplaySize().height())
            .arg(metadata_.taggedColorPrimaries.isEmpty() ? QStringLiteral("untagged")
                                                          : metadata_.taggedColorPrimaries)
            .arg(metadata_.taggedColorTransfer.isEmpty() ? QStringLiteral("untagged")
                                                         : metadata_.taggedColorTransfer)
            .arg(metadata_.taggedColorMatrix.isEmpty() ? QStringLiteral("untagged")
                                                       : metadata_.taggedColorMatrix)
            .arg(!metadata_.hasTaggedRange ? QStringLiteral("untagged")
                 : (metadata_.taggedRangeIsFull ? QStringLiteral("full")
                                                : QStringLiteral("limited")))
            .arg(metadata_.containerName.isEmpty() ? QStringLiteral("?") : metadata_.containerName)
            .arg(metadata_.codecProfile.isEmpty() ? QStringLiteral("none") : metadata_.codecProfile)
            .arg(metadata_.videoTrackId)
            .arg(metadata_.hasAudio
                     ? QStringLiteral("%1/%2Hz/%3/%4")
                           .arg(metadata_.audioCodecName)
                           .arg(metadata_.audioSampleRate)
                           .arg(metadata_.audioChannelLayout)
                           .arg(metadata_.audioTrackId)
                     : QStringLiteral("none"))
            .arg(metadata_.pixelFormatName.isEmpty() ? QStringLiteral("?")
                                                     : metadata_.pixelFormatName)
            .arg(metadata_.bitsPerComponent)
            .arg(metadata_.bitsPerPixel));
    }

    error.clear();
    return true;
#else
    Q_UNUSED(path);
    error = "FFmpeg support not enabled at build time.";
    return false;
#endif
}

bool VideoDecoderFFmpeg::decodeFrameAt(long long frameIndex, VideoFrame& outFrame, QString& error, RequestMode mode) {
#ifdef TRACE_WITH_FFMPEG
    // Per request, so a caller reading it after a false return is reading this
    // request's answer and not the previous one's.
    lastRequestAbandoned_ = false;
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

    auto pushReverseCache = [&](const VideoFrame& frame) {
        if (frame.frameIndex < 0 || frame.isNull()) return;

        if (!impl_->reverseCache.empty()
            && impl_->reverseCache.back().frameIndex == frame.frameIndex) {
            // A full-res entry always wins: it can serve every caller, a
            // half-res one cannot. Never downgrade an existing entry.
            if (!frame.previewRes || impl_->reverseCache.back().previewRes) {
                impl_->reverseCacheBytes -= impl_->reverseCache.back().sizeInBytes();
                impl_->reverseCache.back() = frame;
                impl_->reverseCacheBytes += frame.sizeInBytes();
            }
            return;
        }

        // Learn what a full-resolution entry really costs, from one that exists.
        // Only full-res entries teach anything: a preview is a different size on
        // purpose and is priced separately by entriesThatFit.
        if (!frame.previewRes && frame.sizeInBytes() > 0
            && frame.sizeInBytes() != impl_->fullResEntryBytes) {
            impl_->fullResEntryBytes = frame.sizeInBytes();
            impl_->reverseCacheCapacity = std::clamp(
                static_cast<int>(impl_->reverseCacheBudgetBytes / impl_->fullResEntryBytes),
                4, 256);
        }

        impl_->reverseCache.push_back(frame);
        impl_->reverseCacheBytes += frame.sizeInBytes();
        ++perfStats_.cacheInserts;
        // Evict by footprint. The last entry is never dropped: it is the one
        // just decoded, and an empty cache is worse than an over-budget one by
        // a single frame.
        while (impl_->reverseCacheBytes > impl_->reverseCacheBudgetBytes
               && impl_->reverseCache.size() > 1) {
            impl_->reverseCacheBytes -= impl_->reverseCache.front().sizeInBytes();
            impl_->reverseCache.pop_front();
            ++perfStats_.cacheEvictions;
        }
        reportCacheState();
    };

    // Only a Scrub preview may be served a half-res entry; everything else is
    // inspecting the frame and must get full resolution.
    const bool acceptPreviewRes = (mode == RequestMode::Scrub);
    // What this request is expected to produce. Used only to size the seek-walk
    // fill window below, which has to be decided before any frame exists -- the
    // authoritative tag is set by convertCurrentFrame from the size it actually
    // converted at and travels on the frame, so a wrong guess here costs a
    // slightly mis-sized window and can no longer mislabel an entry.
    const bool producesPreviewRes = (mode == RequestMode::Scrub) && (metadata_.width > 1920);
    // Entries of the size this request stores that fit the byte budget. A
    // preview is a quarter of a full-res frame at worst and much less than that
    // when it is converted to the displayed size, so this has to price the size
    // actually being produced -- assuming the full-res worst case is exactly
    // the mistake the byte budget exists to correct.
    auto entriesThatFit = [&](bool previewRes) {
        // Full-res entries are priced from what one really costs (planar since
        // GATE C, and a third to a half of the BGRA figure this used to use).
        // Previews stay on swscale and are still BGRA, so the preview branch
        // keeps computing from w*h*4 -- that one is not stale.
        long long bytes = impl_->fullResEntryBytes > 0
            ? impl_->fullResEntryBytes
            : static_cast<long long>(metadata_.width) * metadata_.height * 4;
        if (previewRes) {
            bytes = static_cast<long long>(metadata_.width) * metadata_.height * 4;
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
            if (it->frameIndex != wantedFrame) continue;
            if (it->previewRes && !acceptPreviewRes) return false;
            ++perfStats_.reverseCacheHits;
            outFrame = *it;
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

        perfStats_.threadTypeIsFrame = impl_->threadTypeIsFrame;
        // Read back off the codec context rather than recomputed from the env,
        // so it reports what FFmpeg is actually running with. A requested count
        // and an applied count are not the same claim.
        perfStats_.threadCount = impl_->codec ? impl_->codec->thread_count : 0;
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
    // Returns false when no frame was produced. The caller must not present or
    // cache the output in that case: `frame` is frequently the same object
    // across requests (MainWindow holds one per media), so leaving the previous
    // frame in it and labelling it with the new index would put one frame on
    // screen under another's name -- the failure the drain fix (e76eabb) exists
    // to prevent. Clearing on entry makes the stale frame unreachable rather
    // than merely unused.
    auto convertCurrentFrame = [&](VideoFrame& frame, bool fastOverride = false,
                                   bool isCacheFill = false) -> bool {
        frame = VideoFrame{};
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
        // Set by the branch below rather than predicted alongside it. The tag
        // travels on the frame, so an entry can no longer be stored at one
        // resolution and labelled another -- previously two separate
        // expressions had to agree, and they read different widths (the decoded
        // frame's here, the container metadata's at the call site).
        bool previewResolution = false;
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
            previewResolution = true;
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

        // Read the matrix/range off the frame rather than the codec context:
        // the frame is what was actually decoded, and container-level metadata
        // is not always propagated before the first frame arrives.
        //
        // Hoisted above the planar branch because the planar path needs exactly
        // the same answer -- it is the shader rather than swscale that applies
        // it, but it is the same decision and must not become two.
        {
            AVColorSpace spc0 = impl_->frame->colorspace;
            if (spc0 == AVCOL_SPC_UNSPECIFIED) spc0 = impl_->codec->colorspace;
            AVColorRange range0 = impl_->frame->color_range;
            if (range0 == AVCOL_RANGE_UNSPECIFIED) range0 = impl_->codec->color_range;
            bool inferred0 = false;
            const int coeff0 = swsCoefficientsFor(spc0, w, h, &inferred0);
            const bool full0 = (range0 == AVCOL_RANGE_JPEG);

            // Planar handoff: no scale, no matrix, no swscale. Only for frames
            // wanted at full resolution -- a scrub preview is converted to the
            // displayed size and is far cheaper as BGRA (plan section 20.7).
            PixelLayout planarLayout = PixelLayout::Unknown;
            int planarDepth = 0;
            const ColorInfo planarColor = colorInfoFor(coeff0, full0, inferred0);
            // Only the three matrices the shader has exact coefficients for.
            // Fcc and Smpte240m would have to be approximated, and an
            // approximation here is a colour difference between the two
            // backends that no A/B could attribute to anything -- swscale
            // applies them properly, so those files keep taking that path.
            const bool matrixSupported = planarColor.matrix == ColorInfo::Matrix::BT601
                                      || planarColor.matrix == ColorInfo::Matrix::BT709
                                      || planarColor.matrix == ColorInfo::Matrix::BT2020;
            if (planarOutput_ && matrixSupported && !previewResolution && dstW == w && dstH == h
                && planarLayoutFor(srcPixFmt, &planarLayout, &planarDepth)) {
                QElapsedTimer palloc;
                palloc.start();
                std::shared_ptr<FrameBuffer> target =
                    impl_->acquirePlanarTarget(w, h, planarLayout, planarDepth);
                if (!target) {
                    target = FrameBuffer::allocatePlanar(w, h, planarLayout, planarDepth);
                }
                convertAllocNs += palloc.nsecsElapsed();
                if (target) {
                    QElapsedTimer copyTimer;
                    copyTimer.start();
                    const int sampleBytes = target->bytesPerSample();
                    for (int p = 0; p < 3; ++p) {
                        const uint8_t* src = impl_->frame->data[p];
                        if (!src) { target = nullptr; break; }
                        const int srcStride = impl_->frame->linesize[p];
                        uint8_t* dst = target->data(p);
                        const int dstStride = target->bytesPerLine(p);
                        const int rowBytes = target->planeWidth(p) * sampleBytes;
                        const int rows = target->planeHeight(p);
                        // Row by row: FFmpeg's linesize is padded for SIMD and
                        // rarely equals ours, so a single memcpy of the plane
                        // would shear the picture.
                        for (int y = 0; y < rows; ++y) {
                            std::memcpy(dst + static_cast<ptrdiff_t>(y) * dstStride,
                                        src + static_cast<ptrdiff_t>(y) * srcStride,
                                        static_cast<size_t>(rowBytes));
                        }
                    }
                    if (target) {
                        const qint64 copyNs = copyTimer.nsecsElapsed();
                        swsScaleNs += copyNs;
                        if (isCacheFill) cacheConvertNs += copyNs;
                        else convertNs += copyNs;

                        perfStats_.colorMatrix = QString::fromLatin1(swsCoefficientsName(coeff0));
                        perfStats_.colorMatrixInferred = inferred0;
                        perfStats_.srcFullRange = full0;
                        perfStats_.lastImageWasShared = false;
                        perfStats_.fullFrameCopiesPerFrame = 1;
                        perfStats_.swsContextReused = true;
                        perfStats_.experimentalFastPathEnabled = useFast;
                        perfStats_.swsSlotRebuilds = impl_->swsRebuilds;
                        perfStats_.swsSlotsInUse = impl_->swsSlotsInUse();
                        perfStats_.dstPixelFormat =
                            QStringLiteral("%1P%2 planar")
                                .arg(planarLayout == PixelLayout::YUV420P ? "YUV420"
                                     : planarLayout == PixelLayout::YUV422P ? "YUV422"
                                                                            : "YUV444")
                                .arg(planarDepth);

                        frame.buffer = std::move(target);
                        frame.color = planarColor;
                        frame.previewRes = false;
                        return true;
                    }
                    // A plane was missing. Fall through to swscale rather than
                    // publishing a frame with two of three planes filled.
                }
            }
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
            return false;
        }

        // AV_PIX_FMT_BGRA writes bytes B,G,R,A; on little-endian that is
        // exactly QImage::Format_RGB32's 0xffRRGGBB layout (alpha ignored).
        constexpr PixelLayout kFrameLayout = PixelLayout::BGRA8;

        // Convert into a buffer nothing else holds. The pool hands back one
        // whose only holder is the pool itself; the frame published below is
        // what gives anyone else a reference.
        QElapsedTimer allocTimer;
        allocTimer.start();
        std::shared_ptr<FrameBuffer> target = impl_->acquireConvertTarget(dstW, dstH, kFrameLayout);
        if (!target) {
            // More outstanding references than the pool expects. Correctness
            // must not depend on pool sizing, so use a private buffer.
            target = FrameBuffer::allocate(dstW, dstH, kFrameLayout);
        }
        convertAllocNs += allocTimer.nsecsElapsed();

        if (!target) {
            return false;
        }

        // Writing through the buffer cannot detach: the pool only yields one it
        // solely owns, and a VideoFrame holding it is a refcount rather than a
        // copy-on-write alias. The old pooled-QImage path had to time bits()
        // because it could deep-copy ~38MB here; these two counters are kept so
        // a regression back to that behaviour would still be visible, and they
        // now read zero by construction.
        perfStats_.lastImageWasShared = false;
        uint8_t* const dstBits = target->data();

        uint8_t* dstData[4] = { dstBits, nullptr, nullptr, nullptr };
        int dstLinesize[4] = { target->bytesPerLine(), 0, 0, 0 };

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

        // Publish. Holding the frame is a refcount on the pooled buffer, so the
        // pool will not reuse it until every holder -- viewer, cache, an
        // in-flight request -- has released it. The frame index is filled in by
        // the caller, which is the only place that knows which frame this is.
        frame.buffer = std::move(target);
        frame.color = colorInfoFor(coefficients, srcFullRange, matrixInferred);
        frame.previewRes = previewResolution;

        perfStats_.fullFrameCopiesPerFrame = 1;
        // Carry the actual conversion size: "half" stopped being the whole
        // story once previews are converted straight to the displayed size.
        perfStats_.dstPixelFormat = (dstW != w)
            ? QStringLiteral("RGB32/BGRA %1x%2").arg(dstW).arg(dstH)
            : QStringLiteral("RGB32/BGRA");
        return true;
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
    //
    // Three outcomes, not two. "The frame is not there" and "the frame is no
    // longer wanted" look identical to a bool and are completely different
    // things: only the first is a decode failure, and only the first should
    // provoke the recovery seek.
    enum class WalkResult { Produced, Exhausted, Abandoned };
    // How long the walk goes between consecutive chances to notice a
    // cancellation. This is the decoder's contribution to cancellation
    // latency, and it is the number that says whether the checkpoint is at the
    // right granularity -- the worker measures the rest.
    QElapsedTimer checkpointClock;
    qint64 lastCheckpointNs = -1;
    double maxCheckpointGapMs = 0.0;
    auto decodeUntilTarget = [&](long long target) -> WalkResult {
        // Reset per call, not per request: the recovery path runs this twice
        // and the second run's first gap would otherwise be measured against
        // the first run's last checkpoint, with a seek in between.
        checkpointClock.start();
        lastCheckpointNs = -1;
        while (true) {
            const qint64 checkNs = checkpointClock.nsecsElapsed();
            if (lastCheckpointNs >= 0) {
                maxCheckpointGapMs = std::max(maxCheckpointGapMs,
                    static_cast<double>(checkNs - lastCheckpointNs) / 1'000'000.0);
            }
            lastCheckpointNs = checkNs;

            // The one cancellation checkpoint, and the only place in this
            // function where the decoder is quiescent enough for one: no
            // AVPacket is owned (every path above unrefs before looping),
            // impl_->frame is not being written, the codec is between
            // send/receive cycles and no conversion is running. Reached once
            // per packet, which on long-GOP H.264 is every ~0.5-3ms.
            //
            // Do not move this into the receive loop. That loop normally
            // yields one frame and then EAGAIN, so it returns here anyway --
            // a check there would buy no latency and would sit in the middle
            // of a codec state transition.
            if (impl_->cancelPredicate && impl_->cancelPredicate()) {
                ++perfStats_.walksAbandoned;
                return WalkResult::Abandoned;
            }

            // Nothing left in the codec: the caller asked for a frame past the
            // real end of the stream.
            if (impl_->decoderFullyDrained) return WalkResult::Exhausted;

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
                        VideoFrame cached;
                        if (convertCurrentFrame(cached, /*fastOverride=*/true,
                                                /*isCacheFill=*/true)) {
                            cached.frameIndex = decodedFrame;
                            pushReverseCache(cached);
                        }
                        ++walkCacheConverts;
                    }

                    continue;
                }

                // decodedFrame >= target: convert and return it. Frames are
                // emitted in presentation order, so an overshoot means the
                // exact target does not exist in the stream; showing the
                // first frame at/after the target keeps ordering stable.
                if (!convertCurrentFrame(outFrame)) {
                    // The frame was decoded but could not be converted. Report
                    // the miss rather than returning the previous frame under
                    // this one's index.
                    error = "Frame conversion failed";
                    ++perfStats_.staleSuccessPrevented;
                    return WalkResult::Exhausted;
                }
                outFrame.frameIndex = decodedFrame;
                currentFrame_ = decodedFrame;
                perfStats_.previewApproximate = false;
                perfStats_.previewTargetFrame = target;
                perfStats_.previewDisplayedFrame = decodedFrame;
                // Tagged rather than withheld: a soft entry is exactly what the
                // next backward drag step wants and exactly what a paused step
                // must not be given, and tryReverseCache enforces that. At or
                // below 1920px nothing is halved, so the entry is full-res and
                // serves everything.
                pushReverseCache(outFrame);
                return WalkResult::Produced;
            }

            // Draining and the codec yielded nothing this pass: there is no
            // further input to feed it, so stop rather than spin on EAGAIN.
            if (impl_->drainPacketSent && !impl_->decoderFullyDrained) {
                impl_->decoderFullyDrained = true;
            }
        }
        // The requested frame was not produced. Returning true here (because
        // outFrame still held the *previous* frame) is what made the viewer
        // repeat the tail frame and re-seek once per tick past EOF.
        ++perfStats_.staleSuccessPrevented;
        return WalkResult::Exhausted;
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
    // Widened for long-GOP, and only there. The forward shuttle asks for frames
    // `stride` apart, and on a long-GOP codec walking that gap forward is much
    // cheaper than the seek it would otherwise take: at 4K H.264 a walked frame
    // is ~2.6ms against a ~30ms seek plus its own walk back up from the
    // keyframe, so anything up to about a GOP is worth walking.
    //
    // INTRA-ONLY IS 1, WHICH IS THE SAME REASONING TAKEN ONE STEP FURTHER THAN IT
    // USED TO BE. There a seek lands directly on the target for the price of one
    // decode, so walking N frames to avoid a free seek costs N decodes -- and
    // that argument does not stop at 30, it applies at 2. A limit of 1 means
    // ordinary sequential playback (+1) still walks, as it must, and any jump
    // seeks.
    //
    // It was 4, chosen when the only thing that jumped was the audio clock
    // correcting a frame or two of drift and the cost of being wrong was small.
    // The real-time frame drop (MainWindow::realtimeDropSteps) made it the hot
    // path instead, and 4 turned every dropped frame into a decoded one: the 8K
    // ProRes 4444 XQ plate read `walk 3f`, `dec 215ms` and **14.4% of real time**,
    // WORSE than not dropping at all, because a present that skips 3 frames was
    // decoding all 3. It is a runaway as well as a cost -- slower playback falls
    // further behind, which asks for a bigger jump, which is slower still. With
    // the seek it reads `walk 0f` and holds media time.
    //
    // Long-GOP keeps 48 and must: a walked frame there is ~2.6ms against a ~30ms
    // seek plus its own walk up from the keyframe, so the trade genuinely inverts
    // between the two codec families. `intraOnly` is the codec's own answer --
    // the same discriminator the scrub sampling gate uses, and the only one of
    // the five candidates that has ever measured right.
    //
    // IT IS CONDITIONED ON THE THREADING MODE, BECAUSE THAT IS WHAT SETS THE PRICE
    // OF A SEEK -- and that conditioning is the surviving half of a per-mode
    // threading switch that was built, measured, and taken back out.
    //
    // Frame threading must refill ~thread_count packets after every flush: on the
    // 8K plate a frame-threaded seek measures 116ms against a slice-threaded
    // ~30ms. So the two 2026-08-13 changes collide head-on -- frame threading for
    // playback wants to WALK, and the real-time drop wants to SEEK. All four
    // cells, 8K ProRes 4444 XQ, and three of them are catastrophic:
    //
    //   frame + seek    4.2% of real time, media 13.5%   dec 658ms  refill per frame
    //   slice + walk   14.4%,              media 53.5%   dec 215ms  decodes every drop
    //   frame + walk   16.7%,              media 64.4%   handler 253ms
    //   slice + seek   46.0%,              media 97.3%   dec  60ms   <-- and not close
    //
    // WHICH IS WHY FRAME THREADING FOR PLAYBACK DID NOT SHIP, against the obvious
    // reading of the trade. Dropping makes playback ask for jumps, and on
    // intra-only a jump IS random access -- so the mode that would want frame
    // threading stops existing exactly when the source is heavy enough to want it.
    // The switch was measured on both reported files: the 8K never earned it, and
    // the marginal 4448x3096 plate earned it and was WORSE for it (92.3% presented
    // with 7 drops, against 98.4% and none, because the one codec reopen costs a
    // 280ms stall). See the CLAUDE.md entry; TRACE_INTRA_FRAME_THREADS=1 is the
    // control that reproduces all of it.
    //
    // The condition is retained rather than folded away because it is still live:
    // TRACE_INTRA_FRAME_THREADS pins frame threading on, and under it the walk is
    // the right answer for exactly the reason above.
    const long long kPlaybackForwardWalkLimit =
        (metadata_.intraOnly && !impl_->threadTypeIsFrame) ? 1 : 48;
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
        cacheWindow = std::min<long long>(impl_->seekWalkCacheWindowOverride,
                                          entriesThatFit(producesPreviewRes));
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

    const WalkResult walk = decodeUntilTarget(frameIndex);
    perfStats_.maxCheckpointGapMs =
        std::max(perfStats_.maxCheckpointGapMs, maxCheckpointGapMs);

    // Abandoned is not failed. The frame was not produced because the caller
    // stopped wanting it, which is neither an error to report nor a
    // mispositioned decoder to recover from -- running the recovery seek here
    // would move `recov` off 0, and `recov` is a correctness counter whose
    // whole value is that it stays there.
    if (walk == WalkResult::Abandoned) {
        lastRequestAbandoned_ = true;
        updatePerfStats();
        error.clear();
        return false;
    }

    if (walk != WalkResult::Produced) {
        // Failing without having seeked means the decoder was not where the
        // walk assumed. The condition above should prevent that, but a decode
        // failure is user-visible and a seek is cheap next to being wrong, so
        // recover rather than report: reposition and decode the frame properly.
        // A failure *after* a seek is honest -- the frame genuinely is not in
        // the stream -- and still returns false rather than a stale image.
        if (!didSeek && seekToFrame(frameIndex)) {
            ++perfStats_.recoveredDecodeFailures;
            const WalkResult retry = decodeUntilTarget(frameIndex);
            if (retry == WalkResult::Abandoned) {
                lastRequestAbandoned_ = true;
                updatePerfStats();
                error.clear();
                return false;
            }
            if (retry == WalkResult::Produced) {
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
    Q_UNUSED(outFrame);
    Q_UNUSED(mode);
    error = "FFmpeg support not enabled at build time.";
    return false;
#endif
}

} // namespace trace::core
