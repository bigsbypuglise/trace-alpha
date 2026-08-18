#include "ui/StripBackdrop.h"

#include "core/VideoFrame.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace trace::ui {
namespace {

using trace::core::PixelLayout;

// One sample read, normalised to 0..255, from a plane that may be 8-bit or
// 16-bit-storage high-bit-depth. The shift rather than a divide is the same
// reasoning the shader's scale factor carries: above 8 bits the sample occupies
// the LOW bits of a 16-bit word, so the top 8 bits of the value are what a
// 0..255 destination wants.
inline int sampleAt(const uint8_t* plane, int stride, int bytesPerSample,
                    int bitDepth, int x, int y) {
    const uint8_t* row = plane + static_cast<ptrdiff_t>(y) * stride;
    if (bytesPerSample == 1) return row[x];
    const uint16_t v = *reinterpret_cast<const uint16_t*>(row + static_cast<ptrdiff_t>(x) * 2);
    const int shift = bitDepth - 8;
    return shift > 0 ? (v >> shift) : v;
}

// The two matrices Trace actually tags frames with. Fcc and Smpte240m are
// DECLINED everywhere else in this project because they have no exact
// coefficients (GATE C), and they are not special-cased here either -- they
// fall to 709, which for a backdrop that is about to be blurred into 48x6 cells
// is a difference nothing can see. Said out loud rather than left as a silent
// default, because "the colour is approximate here" is exactly the kind of
// thing this codebase has been bitten by inheriting.
struct YuvCoeffs { double kr, kb; };
inline YuvCoeffs coeffsFor(trace::core::ColorInfo::Matrix m) {
    using M = trace::core::ColorInfo::Matrix;
    if (m == M::BT601) return {0.299, 0.114};
    if (m == M::BT2020) return {0.2627, 0.0593};
    return {0.2126, 0.0722};  // BT709 and everything unspecified
}

inline uint8_t clamp255(double v) {
    return static_cast<uint8_t>(std::clamp(v, 0.0, 255.0));
}

// Separable 1-2-1 blur over the tiny grid, run twice. At 48x6 this is a few
// thousand operations and removes the cell boundaries the stretch would
// otherwise show as banding.
void blurGrid(QImage& img) {
    const int w = img.width();
    const int h = img.height();
    if (w < 3 || h < 3) return;
    for (int pass = 0; pass < 2; ++pass) {
        QImage tmp = img;
        for (int y = 0; y < h; ++y) {
            const QRgb* src = reinterpret_cast<const QRgb*>(tmp.constScanLine(y));
            QRgb* dst = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const QRgb a = src[std::max(0, x - 1)];
                const QRgb b = src[x];
                const QRgb c = src[std::min(w - 1, x + 1)];
                dst[x] = qRgb((qRed(a) + 2 * qRed(b) + qRed(c)) / 4,
                              (qGreen(a) + 2 * qGreen(b) + qGreen(c)) / 4,
                              (qBlue(a) + 2 * qBlue(b) + qBlue(c)) / 4);
            }
        }
        tmp = img;
        for (int y = 0; y < h; ++y) {
            const QRgb* up = reinterpret_cast<const QRgb*>(tmp.constScanLine(std::max(0, y - 1)));
            const QRgb* mid = reinterpret_cast<const QRgb*>(tmp.constScanLine(y));
            const QRgb* dn = reinterpret_cast<const QRgb*>(tmp.constScanLine(std::min(h - 1, y + 1)));
            QRgb* dst = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                dst[x] = qRgb((qRed(up[x]) + 2 * qRed(mid[x]) + qRed(dn[x])) / 4,
                              (qGreen(up[x]) + 2 * qGreen(mid[x]) + qGreen(dn[x])) / 4,
                              (qBlue(up[x]) + 2 * qBlue(mid[x]) + qBlue(dn[x])) / 4);
            }
        }
    }
}

} // namespace

QImage StripBackdrop::sample(const trace::core::VideoFrame& frame, double coverFraction) {
    const auto& buf = frame.buffer;
    if (!buf) return QImage();

    const int fw = buf->width();
    const int fh = buf->height();
    if (fw <= 0 || fh <= 0) return QImage();

    // The band of the SOURCE the strip sits over. Clamped to at least one row so
    // a very tall frame under a very short strip still samples something.
    const int bandH = std::clamp(static_cast<int>(std::lround(fh * std::clamp(coverFraction, 0.0, 1.0))),
                                 1, fh);

    const PixelLayout layout = buf->layout();
    const bool planar = trace::core::isPlanarYuv(layout);
    if (layout != PixelLayout::BGRA8 && !planar) return QImage();

    QImage out(kCellsX, kCellsY, QImage::Format_ARGB32);
    if (out.isNull()) return QImage();

    const int bps = buf->bytesPerSample();
    const int depth = buf->bitDepth();
    const auto co = coeffsFor(frame.color.matrix);
    const bool full = frame.color.fullRange;

    // Chroma subsampling, as a shift per axis -- exactly the information the
    // layout carries and the only thing that differs between 420, 422 and 444.
    int cxShift = 0, cyShift = 0;
    if (layout == PixelLayout::YUV420P) { cxShift = 1; cyShift = 1; }
    else if (layout == PixelLayout::YUV422P) { cxShift = 1; cyShift = 0; }

    const uint8_t* p0 = buf->data(0);
    const uint8_t* p1 = planar ? buf->data(1) : nullptr;
    const uint8_t* p2 = planar ? buf->data(2) : nullptr;
    if (!p0 || (planar && (!p1 || !p2))) return QImage();
    const int s0 = buf->bytesPerLine(0);
    const int s1 = planar ? buf->bytesPerLine(1) : 0;
    const int s2 = planar ? buf->bytesPerLine(2) : 0;

    for (int cy = 0; cy < kCellsY; ++cy) {
        QRgb* dstRow = reinterpret_cast<QRgb*>(out.scanLine(cy));
        for (int cx = 0; cx < kCellsX; ++cx) {
            double rSum = 0.0, gSum = 0.0, bSum = 0.0;
            int n = 0;
            for (int sy = 0; sy < kSamplesPerCell; ++sy) {
                // Sample at cell centres of a (kCellsY * kSamplesPerCell) grid
                // over the band, so no sample lands on a shared edge and the
                // pattern is identical for every cell.
                const double fy = (cy * kSamplesPerCell + sy + 0.5) / (kCellsY * kSamplesPerCell);
                const int y = std::clamp(static_cast<int>(fy * bandH), 0, bandH - 1);
                for (int sx = 0; sx < kSamplesPerCell; ++sx) {
                    const double fx = (cx * kSamplesPerCell + sx + 0.5) / (kCellsX * kSamplesPerCell);
                    const int x = std::clamp(static_cast<int>(fx * fw), 0, fw - 1);

                    if (!planar) {
                        const uint8_t* px = p0 + static_cast<ptrdiff_t>(y) * s0 + static_cast<ptrdiff_t>(x) * 4;
                        bSum += px[0]; gSum += px[1]; rSum += px[2];
                    } else {
                        const int Y = sampleAt(p0, s0, bps, depth, x, y);
                        const int cxs = std::min(x >> cxShift, std::max(0, buf->planeWidth(1) - 1));
                        const int cys = std::min(y >> cyShift, std::max(0, buf->planeHeight(1) - 1));
                        const int U = sampleAt(p1, s1, bps, depth, cxs, cys);
                        const int V = sampleAt(p2, s2, bps, depth, cxs, cys);

                        // Range normalisation before the matrix, which is the
                        // same order the shader uses and is exact because both
                        // steps are affine.
                        double yy = full ? (Y / 255.0) : ((Y - 16.0) / 219.0);
                        double u = full ? ((U - 128.0) / 255.0) : ((U - 128.0) / 224.0);
                        double v = full ? ((V - 128.0) / 255.0) : ((V - 128.0) / 224.0);
                        const double kr = co.kr, kb = co.kb, kg = 1.0 - kr - kb;
                        const double r = yy + 2.0 * (1.0 - kr) * v;
                        const double b = yy + 2.0 * (1.0 - kb) * u;
                        const double g = (yy - kr * r - kb * b) / kg;
                        rSum += r * 255.0; gSum += g * 255.0; bSum += b * 255.0;
                    }
                    ++n;
                }
            }
            const double inv = n > 0 ? 1.0 / n : 0.0;
            dstRow[cx] = qRgb(clamp255(rSum * inv), clamp255(gSum * inv), clamp255(bSum * inv));
        }
    }

    blurGrid(out);
    return out;
}

} // namespace trace::ui
