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
// undefined. The planar YUV layouts the GPU renderer will want are not listed
// yet -- add them when something can actually produce one.
enum class PixelLayout {
    Unknown,
    // 32bpp, byte order B,G,R,A. On little-endian that is exactly
    // QImage::Format_RGB32's 0xffRRGGBB, and AV_PIX_FMT_BGRA's byte order.
    BGRA8,
};

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
// Exactly one plane today. Planar YUV upload (GPU phase 7) adds more; the
// accessors are deliberately shaped so that is an addition rather than a
// change of meaning.
class FrameBuffer {
public:
    // Allocates writable storage. `bytesPerLine` matches QImage's stride rule
    // for the equivalent format, so swscale sees the same destination geometry
    // it did before this type existed.
    static std::shared_ptr<FrameBuffer> allocate(int width, int height, PixelLayout layout);

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
    uint8_t* data() { return data_; }
    const uint8_t* data() const { return data_; }

    int width() const { return width_; }
    int height() const { return height_; }
    int bytesPerLine() const { return bytesPerLine_; }
    PixelLayout layout() const { return layout_; }
    // Footprint for the frame cache's byte budget. Counts the allocation, not
    // width*height*4, so a padded stride is priced honestly.
    long long sizeInBytes() const { return static_cast<long long>(bytesPerLine_) * height_; }

private:
    FrameBuffer() = default;

    uint8_t* data_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int bytesPerLine_ = 0;
    PixelLayout layout_ = PixelLayout::Unknown;
    // Set when the storage came from adopt(); its lifetime is the QImage's, not
    // an aligned allocation of ours.
    QImage adopted_;
    bool ownsAllocation_ = false;
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
