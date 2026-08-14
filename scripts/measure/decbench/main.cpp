// Decode-only benchmark against the EXACT FFmpeg libraries Trace links.
//
// No conversion, no upload, no render, no seek. Reads the file sequentially once
// per configuration and decodes every frame, so the number it reports is an upper
// bound on what Trace's pipeline could present.
//
// One persistent decoder per configuration for the whole file: the codec is
// opened once, all frames are decoded, then it is closed. Nothing is reopened
// between samples within a run.
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/cpu.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

using Clock = std::chrono::steady_clock;
static double ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

struct Result {
    double wallS = 0.0;
    long long frames = 0;
    double readMs = 0.0;
    double sendMs = 0.0;
    double recvMs = 0.0;
    std::vector<double> perFrameMs;
};

static bool runOne(const char* path, int threadType, int threadCount,
                   int skipAlpha, Result& out) {
    AVFormatContext* fmt = nullptr;
    if (avformat_open_input(&fmt, path, nullptr, nullptr) < 0) return false;
    if (avformat_find_stream_info(fmt, nullptr) < 0) { avformat_close_input(&fmt); return false; }

    int vs = -1;
    for (unsigned i = 0; i < fmt->nb_streams; ++i)
        if (fmt->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) { vs = (int)i; break; }
    if (vs < 0) { avformat_close_input(&fmt); return false; }

    const AVCodec* dec = avcodec_find_decoder(fmt->streams[vs]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(dec);
    avcodec_parameters_to_context(ctx, fmt->streams[vs]->codecpar);
    ctx->thread_type = threadType;
    ctx->thread_count = threadCount;
    if (skipAlpha) av_opt_set_int(ctx->priv_data, "skip_alpha", 1, 0);
    if (avcodec_open2(ctx, dec, nullptr) < 0) {
        avcodec_free_context(&ctx); avformat_close_input(&fmt); return false;
    }

    AVPacket* pkt = av_packet_alloc();
    AVFrame* frame = av_frame_alloc();
    const auto t0 = Clock::now();
    auto lastOut = t0;

    for (;;) {
        const auto r0 = Clock::now();
        const int rr = av_read_frame(fmt, pkt);
        out.readMs += ms(Clock::now() - r0);
        if (rr < 0) break;
        if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }

        const auto s0 = Clock::now();
        avcodec_send_packet(ctx, pkt);
        out.sendMs += ms(Clock::now() - s0);
        av_packet_unref(pkt);

        for (;;) {
            const auto c0 = Clock::now();
            const int rc = avcodec_receive_frame(ctx, frame);
            out.recvMs += ms(Clock::now() - c0);
            if (rc < 0) break;
            const auto now = Clock::now();
            out.perFrameMs.push_back(ms(now - lastOut));
            lastOut = now;
            ++out.frames;
            av_frame_unref(frame);
        }
    }
    avcodec_send_packet(ctx, nullptr);
    for (;;) {
        if (avcodec_receive_frame(ctx, frame) < 0) break;
        const auto now = Clock::now();
        out.perFrameMs.push_back(ms(now - lastOut));
        lastOut = now;
        ++out.frames;
        av_frame_unref(frame);
    }
    out.wallS = ms(Clock::now() - t0) / 1000.0;

    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
    return true;
}

static double pct(std::vector<double> v, double p) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t i = (size_t)(p * (v.size() - 1));
    return v[i];
}

int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: decbench <file> <srcfps>\n"); return 2; }
    const char* path = argv[1];
    const double srcFps = atof(argv[2]);

    std::printf("avcodec %u.%u.%u | cpu_flags 0x%X | av_cpu_count %d\n",
                avcodec_version() >> 16, (avcodec_version() >> 8) & 255,
                avcodec_version() & 255, av_get_cpu_flags(), av_cpu_count());

    // Report what the stream actually is, from the container.
    {
        AVFormatContext* fmt = nullptr;
        if (avformat_open_input(&fmt, path, nullptr, nullptr) == 0) {
            avformat_find_stream_info(fmt, nullptr);
            for (unsigned i = 0; i < fmt->nb_streams; ++i) {
                AVCodecParameters* p = fmt->streams[i]->codecpar;
                if (p->codec_type != AVMEDIA_TYPE_VIDEO) continue;
                const AVCodec* d = avcodec_find_decoder(p->codec_id);
                const AVPixFmtDescriptor* pd = av_pix_fmt_desc_get((AVPixelFormat)p->format);
                std::printf("stream  : %s profile=%d %dx%d pix=%s depth=%d planes=%d "
                            "log2chroma=%d/%d alpha=%d\n",
                            d ? d->name : "?", p->profile, p->width, p->height,
                            pd ? pd->name : "?", pd ? pd->comp[0].depth : 0,
                            pd ? av_pix_fmt_count_planes((AVPixelFormat)p->format) : 0,
                            pd ? pd->log2_chroma_w : 0, pd ? pd->log2_chroma_h : 0,
                            pd ? ((pd->flags & AV_PIX_FMT_FLAG_ALPHA) ? 1 : 0) : 0);
                std::printf("rate    : %d/%d  bitrate=%lld\n",
                            fmt->streams[i]->r_frame_rate.num,
                            fmt->streams[i]->r_frame_rate.den,
                            (long long)p->bit_rate);
                std::printf("decoder : %s  caps=0x%X (FRAME_THREADS=%d SLICE_THREADS=%d)\n",
                            d ? d->name : "?", d ? d->capabilities : 0,
                            d ? ((d->capabilities & AV_CODEC_CAP_FRAME_THREADS) ? 1 : 0) : 0,
                            d ? ((d->capabilities & AV_CODEC_CAP_SLICE_THREADS) ? 1 : 0) : 0);
            }
            avformat_close_input(&fmt);
        }
    }

    struct Cfg { const char* name; int type; int count; int skipAlpha; };
    std::vector<Cfg> cfgs = {
        {"slice",        FF_THREAD_SLICE, 0, 0},
        {"slice t=8",    FF_THREAD_SLICE, 8, 0},
        {"slice t=16",   FF_THREAD_SLICE, 16, 0},
        {"slice t=32",   FF_THREAD_SLICE, 32, 0},
        {"frame",        FF_THREAD_FRAME, 0, 0},
        {"frame t=8",    FF_THREAD_FRAME, 8, 0},
        {"frame t=16",   FF_THREAD_FRAME, 16, 0},
        {"frame+slice",  FF_THREAD_FRAME | FF_THREAD_SLICE, 0, 0},
        {"slice a-skip", FF_THREAD_SLICE, 0, 1},
        {"frame a-skip", FF_THREAD_FRAME, 0, 1},
        {"single",       0, 1, 0},
    };

    std::printf("\n%-14s %7s %7s %8s %8s %8s %8s %8s %8s\n",
                "config", "frames", "wall s", "fps", "%RT", "p50 ms", "p95 ms", "max ms", "read ms");
    std::printf("%s\n", std::string(96, '-').c_str());

    for (const auto& c : cfgs) {
        Result r;
        if (!runOne(path, c.type, c.count, c.skipAlpha, r)) {
            std::printf("%-14s FAILED\n", c.name);
            continue;
        }
        const double fps = r.frames / r.wallS;
        std::printf("%-14s %7lld %7.2f %8.2f %8.1f %8.2f %8.2f %8.2f %8.0f\n",
                    c.name, r.frames, r.wallS, fps, 100.0 * fps / srcFps,
                    pct(r.perFrameMs, 0.50), pct(r.perFrameMs, 0.95),
                    pct(r.perFrameMs, 1.00), r.readMs);
        std::fflush(stdout);
    }
    return 0;
}
