// exrprobe -- read an EXR's channel names IN THE ORDER OpenImageIO PRESENTS
// THEM, plus per-channel value ranges.
//
// It exists because the channel grouper must be written against measured
// ground truth rather than against the EXR spec's alphabetical storage order:
// OIIO's EXR reader may reorder channels, and a grouper written for the wrong
// order finds nothing and reports a multilayer file as plain RGB.

#include <OpenImageIO/imageio.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: exrprobe <file.exr> [--stats]\n");
        return 2;
    }
    const bool stats = (argc > 2 && std::string(argv[2]) == "--stats");
    // --cmp i j : are two channels the same picture? Needed because a root RGB
    // and a named Beauty layer can be the same render written twice, and lossy
    // compression makes "identical" a measurement rather than a yes/no.
    const bool cmp = (argc > 4 && std::string(argv[2]) == "--cmp");
    const int cmpA = cmp ? std::atoi(argv[3]) : -1;
    const int cmpB = cmp ? std::atoi(argv[4]) : -1;

    auto in = OIIO::ImageInput::open(argv[1]);
    if (!in) {
        std::printf("FAIL open: %s\n", OIIO::geterror().c_str());
        return 3;
    }
    const OIIO::ImageSpec& spec = in->spec();
    std::printf("file      %s\n", argv[1]);
    std::printf("size      %dx%d\n", spec.width, spec.height);
    std::printf("nchannels %d\n", spec.nchannels);
    std::printf("format    %s\n", spec.format.c_str());
    std::printf("alpha_ch  %d   z_ch %d\n", spec.alpha_channel, spec.z_channel);

    std::printf("compression %s\n",
                spec.get_string_attribute("compression", "(none)").c_str());
    const auto* chroma = spec.find_attribute("chromaticities");
    std::printf("chromaticities %s\n", chroma ? "PRESENT" : "ABSENT");

    std::printf("--- channels, in OIIO order ---\n");
    for (int c = 0; c < spec.nchannels; ++c) {
        std::printf("  [%2d] %-32s %s\n", c, spec.channelnames[c].c_str(),
                    (c < (int)spec.channelformats.size()
                         ? spec.channelformats[c].c_str()
                         : spec.format.c_str()));
    }

    std::printf("--- other attributes ---\n");
    for (const auto& p : spec.extra_attribs) {
        std::string v = p.get_string(0);
        if (v.size() > 90) v = v.substr(0, 90) + "...";
        std::printf("  %-40s = %s\n", p.name().c_str(), v.c_str());
    }

    // --read <chbegin> <chend> [reps] : what it costs to decode a CHANNEL RANGE.
    // The point of the measurement is that a multilayer EXR is decoded per
    // channel, so asking for 3 of 27 should cost roughly 3/27 of the work --
    // which is the claim behind reading only the pass being displayed.
    if (argc > 4 && std::string(argv[2]) == "--read") {
        const int cb = std::atoi(argv[3]);
        const int ce = std::atoi(argv[4]);
        const int reps = argc > 5 ? std::atoi(argv[5]) : 5;
        const size_t n = (size_t)spec.width * spec.height * (size_t)(ce - cb);
        std::vector<float> px(n);
        double best = 1e30, sum = 0.0;
        for (int r = 0; r < reps; ++r) {
            in->close();
            in = OIIO::ImageInput::open(argv[1]);
            const auto t0 = std::chrono::steady_clock::now();
            if (!in->read_image(0, 0, cb, ce, OIIO::TypeDesc::FLOAT, px.data())) {
                std::printf("FAIL read: %s\n", in->geterror().c_str());
                return 4;
            }
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - t0).count();
            best = std::min(best, ms);
            sum += ms;
        }
        std::printf("--- read channels [%d,%d) x%d ---\n", cb, ce, reps);
        std::printf("  best %8.2f ms   mean %8.2f ms   floats %zu (%.1f MB)\n",
                    best, sum / reps, n, (double)(n * sizeof(float)) / 1048576.0);
        in->close();
        return 0;
    }

    if (cmp) {
        std::vector<float> px((size_t)spec.width * spec.height * spec.nchannels);
        if (!in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, px.data())) {
            std::printf("FAIL read: %s\n", in->geterror().c_str());
            in->close();
            return 4;
        }
        const size_t n = (size_t)spec.width * spec.height;
        double sum = 0.0;
        float worst = 0.0f;
        size_t exact = 0;
        for (size_t i = 0; i < n; ++i) {
            const float d = std::abs(px[i * spec.nchannels + cmpA]
                                   - px[i * spec.nchannels + cmpB]);
            sum += d;
            worst = std::max(worst, d);
            if (d == 0.0f) ++exact;
        }
        std::printf("--- compare [%d] %s  vs  [%d] %s ---\n", cmpA,
                    spec.channelnames[cmpA].c_str(), cmpB,
                    spec.channelnames[cmpB].c_str());
        std::printf("  mean abs diff %.8f   max abs diff %.8f   bit-identical px %.2f%%\n",
                    sum / (double)n, worst, 100.0 * (double)exact / (double)n);
    }

    if (stats) {
        std::vector<float> px((size_t)spec.width * spec.height * spec.nchannels);
        if (!in->read_image(0, 0, 0, spec.nchannels, OIIO::TypeDesc::FLOAT, px.data())) {
            std::printf("FAIL read: %s\n", in->geterror().c_str());
            in->close();
            return 4;
        }
        std::printf("--- per-channel min / max / mean ---\n");
        const size_t n = (size_t)spec.width * spec.height;
        for (int c = 0; c < spec.nchannels; ++c) {
            float lo = px[c], hi = px[c];
            double sum = 0.0;
            size_t over1 = 0;
            for (size_t i = 0; i < n; ++i) {
                const float v = px[i * spec.nchannels + c];
                lo = std::min(lo, v);
                hi = std::max(hi, v);
                sum += v;
                if (v > 1.0f) ++over1;
            }
            std::printf("  [%2d] %-28s min %12.5f  max %12.5f  mean %10.5f  >1.0 %6.2f%%\n",
                        c, spec.channelnames[c].c_str(), lo, hi, sum / (double)n,
                        100.0 * (double)over1 / (double)n);
        }
    }
    in->close();
    return 0;
}
