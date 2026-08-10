#pragma once

#include <QImage>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace trace::core {

// How a FrameBuffer's bytes are laid out.
//
// Deliberately Trace's own enum rather than AVPixelFormat: this header sits on
// the image-sequence path too, which must compile with TRACE_WITH_FFMPEG
// undefined.
enum class PixelLayout {
    Unknown,
    // 32bpp, byte order B,G,R,A. On little-endian that is exactly
    // QImage::Format_RGB32's 0xffRRGGBB, and AV_PIX_FMT_BGRA's byte order.
    BGRA8,

    // Three separate Y, U, V planes, no alpha -- the GPU upload path (GATE C).
    // The suffix is chroma subsampling; the bit depth is carried separately by
    // FrameBuffer::bitDepth(), because 10- and 12-bit share one layout and one
    // texture format and differ only in a shader constant.
    //
    // There is no alpha variant on purpose. The shader never samples a fourth
    // plane, so ProRes 4444 arrives here as YUV444 with its alpha dropped at the
    // copy -- the same decision alphaStrippedFormat() makes on the CPU path, for
    // the same reason: the destination cannot show it.
    YUV420P,
    YUV422P,
    YUV444P,
};

// True for the three planar YUV layouts above. Written as a function rather
// than a range check so adding a layout cannot silently join or leave the set.
constexpr bool isPlanarYuv(PixelLayout layout) {
    return layout == PixelLayout::YUV420P
        || layout == PixelLayout::YUV422P
        || layout == PixelLayout::YUV444P;
}

// Source colorimetry, carried with the frame rather than looked up again.
//
// On the CPU path swscale has already applied this and the frame is RGB, so
// these are descriptive. On the GPU path they become shader constants (the
// matrices are tabulated in docs/gpu-initiative-plan.md §11), which is why they
// travel with the pixels instead of living only in VideoPerfStats.
struct ColorInfo {
    enum class Matrix {
        Unspecified,
        BT601,
        BT709,
        BT2020,
        Fcc,
        Smpte240m,
    };

    Matrix matrix = Matrix::Unspecified;
    bool fullRange = false;
    // True when the matrix was derived from frame size rather than signalled by
    // the file. Reported in the HUD with a `*` so a wrong-looking picture can be
    // checked against what the file actually claims.
    bool inferred = false;
};

// Reference-counted pixel storage.
//
// This is the recycling conversion buffer that used to be a pooled QImage. The
// reason it is no longer one: QImage::bits() is non-const and detaches, so
// writing into a pooled QImage that the viewer or the frame cache still
// referenced deep-copied ~38MB at 4K, every frame, of data sws_scale was about
// to overwrite in full. The pool avoided that by only ever handing back an
// entry reporting isDetached(); owning the memory outright makes the hazard
// structurally impossible instead of avoided by policy.
//
// One plane for BGRA8, three for the planar YUV layouts. A single allocation
// holds all of them, so the recycling pool, the byte budget and the refcount
// all keep working exactly as they did for one plane.
class FrameBuffer {
public:
    static constexpr int kMaxPlanes = 3;

    // Allocates writable storage. `bytesPerLine` matches QImage's stride rule
    // for the equivalent format, so swscale sees the same destination geometry
    // it did before this type existed.
    static std::shared_ptr<FrameBuffer> allocate(int width, int height, PixelLayout layout);

    // Planar YUV storage for a source of the given bit depth (8, 10 or 12).
    // `width`/`height` are the LUMA dimensions; the chroma planes are derived
    // from the layout's subsampling. Samples above 8 bits are stored as
    // little-endian 16-bit, LSB-aligned, exactly as FFmpeg's p10le/p12le give
    // them -- so the copy in is a memcpy and the bit depth becomes a shader
    // constant rather than a repacking pass.
    static std::shared_ptr<FrameBuffer> allocatePlanar(int width, int height,
                                                       PixelLayout layout, int bitDepth);

    // Takes over an already-decoded image (the still/sequence path, where the
    // pixels come from OIIO or QImage rather than swscale). The image is
    // detached on the way in so this buffer is its sole owner, which keeps the
    // no-aliasing guarantee the allocate() path gets for free.
    static std::shared_ptr<FrameBuffer> adopt(QImage image);

    ~FrameBuffer();

    FrameBuffer(const FrameBuffer&) = delete;
    FrameBuffer& operator=(const FrameBuffer&) = delete;

    // Writable pixels. Only meaningful for allocate()d buffers -- a producer
    // owns its buffer until it publishes a VideoFrame over it.
    //
    // The no-argument forms are plane 0, which is the whole picture for BGRA8
    // and the luma plane for planar YUV. Every existing caller is a BGRA one and
    // keeps working unchanged.
    uint8_t* data() { return planes_[0]; }
    const uint8_t* data() const { return planes_[0]; }
    uint8_t* data(int plane) { return plane >= 0 && plane < planeCount_ ? planes_[plane] : nullptr; }
    const uint8_t* data(int plane) const { return plane >= 0 && plane < planeCount_ ? planes_[plane] : nullptr; }

    int width() const { return width_; }
    int height() const { return height_; }
    int bytesPerLine() const { return bytesPerLine_[0]; }
    int bytesPerLine(int plane) const { return plane >= 0 && plane < planeCount_ ? bytesPerLine_[plane] : 0; }
    // Per-plane dimensions in samples. Equal to the frame size for plane 0 and
    // for every plane of a 4:4:4 layout.
    int planeWidth(int plane) const { return plane >= 0 && plane < planeCount_ ? planeWidth_[plane] : 0; }
    int planeHeight(int plane) const { return plane >= 0 && plane < planeCount_ ? planeHeight_[plane] : 0; }
    int planeCount() const { return planeCount_; }
    PixelLayout layout() const { return layout_; }
    // 8 for BGRA8 and for 8-bit planar; 10 or 12 for high-bit-depth planar. This
    // is the SOURCE depth, not the storage depth: above 8 the samples occupy the
    // low bits of a 16-bit word, which is what makes the shader's scale factor
    // 65535/(2^bitDepth - 1) rather than 1.
    int bitDepth() const { return bitDepth_; }
    // Bytes per sample in storage: 1 at 8 bits, 2 above.
    int bytesPerSample() const { return bitDepth_ > 8 ? 2 : 1; }

    // Footprint for the frame cache's byte budget. Counts the allocation, not
    // width*height*4, so a padded stride is priced honestly -- and so a planar
    // frame, which is a third to a half the size of the BGRA one, is priced as
    // what it is rather than as what it would have converted to.
    long long sizeInBytes() const { return totalBytes_; }

private:
    FrameBuffer() = default;

    uint8_t* planes_[kMaxPlanes] = {nullptr, nullptr, nullptr};
    int bytesPerLine_[kMaxPlanes] = {0, 0, 0};
    int planeWidth_[kMaxPlanes] = {0, 0, 0};
    int planeHeight_[kMaxPlanes] = {0, 0, 0};
    int planeCount_ = 1;
    int width_ = 0;
    int height_ = 0;
    int bitDepth_ = 8;
    long long totalBytes_ = 0;
    PixelLayout layout_ = PixelLayout::Unknown;
    // Set when the storage came from adopt(); its lifetime is the QImage's, not
    // an aligned allocation of ours.
    QImage adopted_;
    // The one allocation every plane points into, and what the destructor
    // frees. Kept separately from planes_[0] because a planar buffer's plane
    // pointers are offsets into it and only this one is the allocation base.
    uint8_t* allocation_ = nullptr;
};

// A decoded frame: pixels plus the identity and colorimetry that make them
// meaningful. Copying one is a refcount bump, so a stale request costs a single
// decrement to discard -- the property the async scrub worker needs.
//
// Frames are renderer-agnostic on purpose. GPU textures are presentation
// scratch uploaded from these; they are never frame identity, so device loss
// re-uploads rather than losing anything.
struct VideoFrame {
    std::shared_ptr<FrameBuffer> buffer;
    long long frameIndex = -1;
    ColorInfo color;
    // Converted at reduced resolution for an in-flight scrub. Fine to shuttle
    // past during a drag, which is showing frames at that size anyway, but must
    // never serve a Step or an exact landing where the frame is being
    // inspected. Tagging rather than withholding is what lets 4K backward drags
    // hit the cache at all.
    bool previewRes = false;

    bool isNull() const { return !buffer; }
    int width() const { return buffer ? buffer->width() : 0; }
    int height() const { return buffer ? buffer->height() : 0; }
    long long sizeInBytes() const { return buffer ? buffer->sizeInBytes() : 0; }

    // Zero-copy read-only view for the CPU renderer. The returned QImage does
    // not own the pixels; a cleanup functor holds a shared_ptr so the buffer
    // outlives every copy of the view. Read-only on purpose: an accidental
    // write deep-copies into QImage's own storage instead of scribbling on a
    // buffer the decoder may already be recycling.
    QImage toQImage() const;
};

} // namespace trace::core
