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
        case PixelLayout::Unknown: break;
    }
    return 0;
}

QImage::Format qtFormatFor(PixelLayout layout) {
    switch (layout) {
        case PixelLayout::BGRA8: return QImage::Format_RGB32;
        case PixelLayout::Unknown: break;
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
    buffer->data_ = raw;
    buffer->width_ = width;
    buffer->height_ = height;
    buffer->bytesPerLine_ = bytesPerLine;
    buffer->layout_ = layout;
    buffer->ownsAllocation_ = true;
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
    buffer->data_ = buffer->adopted_.bits();
    buffer->width_ = buffer->adopted_.width();
    buffer->height_ = buffer->adopted_.height();
    buffer->bytesPerLine_ = static_cast<int>(buffer->adopted_.bytesPerLine());
    buffer->layout_ = layoutForQtFormat(buffer->adopted_.format());
    buffer->ownsAllocation_ = false;
    return buffer;
}

FrameBuffer::~FrameBuffer() {
    if (ownsAllocation_) alignedFree(data_);
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
