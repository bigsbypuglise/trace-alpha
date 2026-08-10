#include "core/VideoFrame.h"

#include <cstdlib>
#include <new>

namespace trace::core {
namespace {

// swscale reads and writes the destination in SIMD-width chunks. QImage aligns
// its own allocations generously for the same reason; matching that here keeps
// conversion cost identical to the pooled-QImage path this replaced.
constexpr std::size_t kAlignment = 64;

void* alignedAlloc(std::size_t bytes) {
    if (bytes == 0) return nullptr;
    // Round up: aligned_alloc requires a size that is a multiple of the
    // alignment, and _aligned_malloc is happier with one.
    const std::size_t rounded = ((bytes + kAlignment - 1) / kAlignment) * kAlignment;
#if defined(_MSC_VER)
    return _aligned_malloc(rounded, kAlignment);
#else
    return std::aligned_alloc(kAlignment, rounded);
#endif
}

void alignedFree(void* p) {
    if (!p) return;
#if defined(_MSC_VER)
    _aligned_free(p);
#else
    std::free(p);
#endif
}

int bytesPerPixel(PixelLayout layout) {
    switch (layout) {
        case PixelLayout::BGRA8: return 4;
        default: break;
    }
    return 0;
}

// Horizontal and vertical chroma divisors for the planar layouts.
void chromaShift(PixelLayout layout, int& shiftX, int& shiftY) {
    switch (layout) {
        case PixelLayout::YUV420P: shiftX = 1; shiftY = 1; break;
        case PixelLayout::YUV422P: shiftX = 1; shiftY = 0; break;
        default:                   shiftX = 0; shiftY = 0; break;   // 4:4:4
    }
}

QImage::Format qtFormatFor(PixelLayout layout) {
    switch (layout) {
        case PixelLayout::BGRA8: return QImage::Format_RGB32;
        // Planar YUV has no QImage equivalent, and returning one that happened
        // to have the right byte count would show the luma plane as garbage
        // rather than failing. The CPU renderer never receives one; the check
        // is here so that stays true by construction.
        default: break;
    }
    return QImage::Format_Invalid;
}

PixelLayout layoutForQtFormat(QImage::Format format) {
    switch (format) {
        case QImage::Format_RGB32:
        case QImage::Format_ARGB32:
        case QImage::Format_ARGB32_Premultiplied:
            return PixelLayout::BGRA8;
        default:
            return PixelLayout::Unknown;
    }
}

} // namespace

std::shared_ptr<FrameBuffer> FrameBuffer::allocate(int width, int height, PixelLayout layout) {
    const int bpp = bytesPerPixel(layout);
    if (width <= 0 || height <= 0 || bpp == 0) return nullptr;

    // QImage's stride rule: round the row up to a 4-byte boundary. For 32bpp
    // that is exactly width*4, so swscale gets the destination geometry it saw
    // before, and a frame's footprint is unchanged.
    const int bytesPerLine = ((width * bpp * 8 + 31) / 32) * 4;

    auto* raw = static_cast<uint8_t*>(
        alignedAlloc(static_cast<std::size_t>(bytesPerLine) * static_cast<std::size_t>(height)));
    if (!raw) return nullptr;

    // Private ctor, so make_shared is not available.
    std::shared_ptr<FrameBuffer> buffer(new FrameBuffer());
    buffer->allocation_ = raw;
    buffer->planes_[0] = raw;
    buffer->planeCount_ = 1;
    buffer->width_ = width;
    buffer->height_ = height;
    buffer->planeWidth_[0] = width;
    buffer->planeHeight_[0] = height;
    buffer->bytesPerLine_[0] = bytesPerLine;
    buffer->bitDepth_ = 8;
    buffer->totalBytes_ = static_cast<long long>(bytesPerLine) * height;
    buffer->layout_ = layout;
    return buffer;
}

std::shared_ptr<FrameBuffer> FrameBuffer::allocatePlanar(int width, int height,
                                                         PixelLayout layout, int bitDepth) {
    if (width <= 0 || height <= 0 || !isPlanarYuv(layout)) return nullptr;
    if (bitDepth != 8 && bitDepth != 10 && bitDepth != 12 && bitDepth != 16) return nullptr;

    int shiftX = 0, shiftY = 0;
    chromaShift(layout, shiftX, shiftY);
    const int sampleBytes = bitDepth > 8 ? 2 : 1;

    // Round up, so an odd width still gets the chroma column that covers its
    // last luma column. FFmpeg allocates the same way, and a plane short by one
    // column reads past the end of the last row on upload.
    const int cw = (width + (1 << shiftX) - 1) >> shiftX;
    const int ch = (height + (1 << shiftY) - 1) >> shiftY;

    const int w[kMaxPlanes] = {width, cw, cw};
    const int h[kMaxPlanes] = {height, ch, ch};

    // 64-byte rows per plane: the upload copies row by row, and a plane start
    // that is not itself aligned would give away the alignment the allocation
    // was made for.
    int stride[kMaxPlanes];
    long long total = 0;
    for (int i = 0; i < kMaxPlanes; ++i) {
        const int rowBytes = w[i] * sampleBytes;
        stride[i] = static_cast<int>(((rowBytes + kAlignment - 1) / kAlignment) * kAlignment);
        total += static_cast<long long>(stride[i]) * h[i];
    }

    auto* raw = static_cast<uint8_t*>(alignedAlloc(static_cast<std::size_t>(total)));
    if (!raw) return nullptr;

    std::shared_ptr<FrameBuffer> buffer(new FrameBuffer());
    buffer->allocation_ = raw;
    buffer->planeCount_ = kMaxPlanes;
    buffer->width_ = width;
    buffer->height_ = height;
    buffer->bitDepth_ = bitDepth;
    buffer->layout_ = layout;
    buffer->totalBytes_ = total;

    long long offset = 0;
    for (int i = 0; i < kMaxPlanes; ++i) {
        buffer->planes_[i] = raw + offset;
        buffer->bytesPerLine_[i] = stride[i];
        buffer->planeWidth_[i] = w[i];
        buffer->planeHeight_[i] = h[i];
        offset += static_cast<long long>(stride[i]) * h[i];
    }
    return buffer;
}

std::shared_ptr<FrameBuffer> FrameBuffer::adopt(QImage image) {
    if (image.isNull()) return nullptr;

    const PixelLayout layout = layoutForQtFormat(image.format());
    if (layout == PixelLayout::Unknown) {
        // Everything downstream draws 32bpp; convert once here rather than
        // leaving a layout the renderer cannot describe.
        image = image.convertToFormat(QImage::Format_RGB32);
        if (image.isNull()) return nullptr;
    }

    std::shared_ptr<FrameBuffer> buffer(new FrameBuffer());
    buffer->adopted_ = std::move(image);
    // Guarantee sole ownership before handing out a raw pointer into it: if the
    // caller's image was still shared, this is the one copy that has to happen,
    // and it happens here rather than unpredictably at first write.
    buffer->adopted_.detach();
    buffer->planes_[0] = buffer->adopted_.bits();
    buffer->planeCount_ = 1;
    buffer->width_ = buffer->adopted_.width();
    buffer->height_ = buffer->adopted_.height();
    buffer->planeWidth_[0] = buffer->width_;
    buffer->planeHeight_[0] = buffer->height_;
    buffer->bytesPerLine_[0] = static_cast<int>(buffer->adopted_.bytesPerLine());
    buffer->bitDepth_ = 8;
    buffer->totalBytes_ =
        static_cast<long long>(buffer->bytesPerLine_[0]) * buffer->height_;
    buffer->layout_ = layoutForQtFormat(buffer->adopted_.format());
    // allocation_ stays null: the storage belongs to the QImage.
    return buffer;
}

FrameBuffer::~FrameBuffer() {
    alignedFree(allocation_);
}

QImage VideoFrame::toQImage() const {
    if (!buffer) return QImage();

    const QImage::Format format = qtFormatFor(buffer->layout());
    if (format == QImage::Format_Invalid) return QImage();

    // Keep the buffer alive for exactly as long as any copy of this view. The
    // holder is deleted by Qt when the last copy goes away, which is also the
    // moment the conversion pool may reuse the buffer.
    auto* holder = new std::shared_ptr<FrameBuffer>(buffer);
    return QImage(
        static_cast<const uint8_t*>(buffer->data()),
        buffer->width(),
        buffer->height(),
        buffer->bytesPerLine(),
        format,
        [](void* h) { delete static_cast<std::shared_ptr<FrameBuffer>*>(h); },
        holder);
}

} // namespace trace::core
