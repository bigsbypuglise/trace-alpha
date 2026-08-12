#pragma once

#include <QImage>
#include <QSize>
#include <QString>

#include "render/D3D11OverlayDrawer.h"
#include "render/VideoRenderer.h"

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace trace::render {

// GATE B: the first native presentation path. A DXGI flip-model swapchain on
// the viewer's own HWND, and one BGRA texture uploaded per frame.
//
// Scope is deliberately narrow -- frame, stride, aspect, resize, fallback --
// and it presents exactly what the CPU backend presents: swscale's BGRA output.
// Planar YUV upload and shader colour conversion are GATE C, and the split is
// drawn here so this commit can be judged on "does a native surface show the
// right pixels at the right size" alone.
//
// It is opt-in (TRACE_RENDERER=d3d11) until GATE E. Any failure to create the
// device, the swapchain or a shader is reported and the app falls back to the
// CPU renderer, because a GPU path that quietly never engages while the app
// looks fine is the failure mode this whole boundary is designed against.
class D3D11VideoRenderer final : public VideoRenderer {
public:
    ~D3D11VideoRenderer() override;

    bool initialize(QWidget* host, QString& error) override;
    void setFrame(const trace::core::VideoFrame& frame) override;
    void clearFrame() override;
    void setPlaceholderText(const QString& text) override;
    void resize(QSize size) override;
    void paint(QWidget* host) override;

    // Qt must stop drawing this widget through its backing store, and must give
    // it a real HWND to hand us. See ViewerWidget: the attributes have to be set
    // before initialize(), and paintEngine() has to return null for the lifetime
    // of the widget.
    bool usesNativeSurface() const override { return true; }

    // Three planes and the matrix in the pixel shader (GATE C). A frame that
    // arrives as BGRA8 anyway -- a scrub preview, a still, or a source whose
    // format or matrix the planar path declines -- still takes the GATE B path,
    // so this is an additional capability rather than a mode.
    bool acceptsPlanarYuv() const override { return true; }

    // Composites the host's overlay model into the same pass as the video, and
    // routes this surface's native mouse input to it. The model owns all the
    // state and every command stays in the application layer -- see
    // OverlayModel and OverlayHooks.
    void setOverlay(OverlayModel* model) override;
    bool overlayDrawFailed() const override { return overlayFailed_; }
    void setCursorHidden(bool hidden) override;

    // Rotate/flip, applied to the texture coordinate in the vertex shader --
    // which is why neither pixel shader knows about it, and why it costs one
    // 2x2 multiply for three vertices rather than anything per pixel.
    void setViewTransform(const ViewTransform& transform) override;
    void setPixelAspect(double par) override;
    void setViewScale(const ViewScale& view) override;

    QString name() const override { return name_; }
    const RenderStats& stats() const override { return stats_; }

private:
    template <typename T>
    using ComPtr = Microsoft::WRL::ComPtr<T>;

    bool createDevice(QString& error);
    // Creates the child window the swapchain presents into. See the definition
    // for why the surface is not simply the host widget's own HWND.
    bool createSurfaceWindow(void* parentHwnd, QSize pixelSize, QString& error);
    bool createSwapChain(void* hwnd, QSize pixelSize, QString& error);
    bool createPipeline(QString& error);
    // Built lazily and only when the size changes, so a steady stream of frames
    // at one resolution reuses everything.
    bool ensureRenderTarget(QString& error);
    bool ensureTexture(int width, int height);
    void uploadPixels(const uint8_t* src, int srcStride, int width, int height);

    // The planar path. Three single-channel textures whose sizes carry the
    // chroma subsampling, so 4:2:0, 4:2:2 and 4:4:4 differ only in how big two
    // of them are and the shader never asks.
    bool ensurePlaneTextures(const trace::core::FrameBuffer& buffer);
    bool uploadPlanes(const trace::core::FrameBuffer& buffer);
    // Range normalisation, bit-depth scale and the 3x3, computed on the CPU for
    // the frame's actual depth and matrix and written to the constant buffer.
    // Returns false for a matrix it has no exact coefficients for, which is the
    // signal to refuse the frame rather than present it through a near-enough
    // one -- the decoder declines the same set, so this should not fire.
    bool updateYuvParams(const trace::core::VideoFrame& frame);
    // Sets the reduction terms from the frame size and the rect it is being drawn
    // into, and re-uploads the constant buffer if they moved. Called from paint()
    // rather than setFrame(), because the destination rect is not known until
    // then -- a resize changes the ratio with no new frame arriving.
    void updateReduction(QSize content, QSize fitted);
    bool uploadYuvParams();
    // Writes the view transform's 2x2 into the vertex shader's constant buffer.
    // Called on change only; the buffer is otherwise untouched between frames.
    bool uploadViewParams();
    void releasePlaneTextures();
    // The empty state. Rendered on the CPU into an image and uploaded through
    // the same path as a frame, so the placeholder survives the move to a
    // native surface instead of quietly becoming a black rectangle.
    void uploadPlaceholder(QSize pixelSize);
    void releaseSizeDependent();

    // The window the swapchain owns. A child of the host widget's HWND rather
    // than the HWND itself.
    HWND surface_ = nullptr;
    // Needed from the window procedure to ask Qt for a repaint when hover or
    // fade changes the picture without a new video frame.
    QWidget* host_ = nullptr;
    D3D11OverlayDrawer overlay_;
    // Non-owning; the host owns it. Null until setOverlay(), and null forever
    // when the overlay is switched off, which is what makes "no overlay" cost
    // one null test per present rather than a disabled draw path.
    OverlayModel* overlayModel_ = nullptr;
    // The drawer's pipeline failed to build. Kept so setOverlay() can refuse
    // the model rather than accepting it and drawing nothing -- the difference
    // matters because paint() would otherwise call buildFrame() every present,
    // rasterising an atlas nobody can display.
    bool overlayFailed_ = false;
    bool mouseTracking_ = false;
    // The surface window has its own class cursor, so hiding the pointer means
    // answering WM_SETCURSOR ourselves rather than calling Qt's setCursor() on a
    // widget this HWND sits above.
    bool cursorHidden_ = false;

public:
    // The window class needs a plain callback; it recovers the instance from
    // GWLP_USERDATA and forwards. Public only because the class registration
    // lives in an anonymous namespace in the .cpp.
    static LRESULT CALLBACK surfaceProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);

private:
    LRESULT handleSurfaceMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, bool& handled);

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> backBufferRtv_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11SamplerState> sampler_;
    // The magnification sampler (spec phase 15). A second STATE, not a second
    // shader and not a second path: the draw picks one of the two by the same
    // predicate updateReduction() gates on, so "which filter" is answered once
    // per paint from the destination rect and every subsampling, bit depth and
    // the box-average reduction stay on the one shader GATE C built.
    ComPtr<ID3D11SamplerState> pointSampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> textureSrv_;

    // GATE C. Kept alongside the BGRA texture rather than replacing it: a
    // session mixes the two frame by frame, because scrub previews stay on
    // swscale, and tearing one set down to build the other on every drag would
    // reallocate three textures per gesture.
    ComPtr<ID3D11PixelShader> yuvPixelShader_;
    ComPtr<ID3D11Texture2D> planeTexture_[3];
    ComPtr<ID3D11ShaderResourceView> planeSrv_[3];
    ComPtr<ID3D11Buffer> yuvParams_;
    // cbuffer ViewParams in FullscreenQuad.vs.hlsl. Bound to the VERTEX stage's
    // b0, which does not collide with the YUV params at the pixel stage's b0.
    ComPtr<ID3D11Buffer> viewParams_;
    ViewTransform viewTransform_{};
    double pixelAspect_ = 1.0;
    ViewScale viewScale_{};

    // Mirrors cbuffer YuvParams in YuvToRgb.ps.hlsl. Held rather than built on
    // the stack because the colour terms come from the frame (setFrame) and the
    // reduction terms from the destination rect (paint), and the two are written
    // at different times into one buffer.
    struct YuvParamsData {
        float matR[4] = {};
        float matG[4] = {};
        float matB[4] = {};
        float sampleScale = 1.0f;
        float lumaOffset = 0.0f;
        float lumaScale = 1.0f;
        float chromaOffset = 0.0f;
        float chromaScale = 1.0f;
        float footprint[2] = {0.0f, 0.0f};
        // 1 == one sample at the pixel centre, i.e. exactly the pre-step-9
        // behaviour. Never 0: the shader clamps, but the default should be the
        // no-op rather than something the shader has to rescue.
        float taps = 1.0f;
    };
    YuvParamsData yuvParamsData_{};

    int planeWidth_[3] = {0, 0, 0};
    int planeHeight_[3] = {0, 0, 0};
    // DXGI_FORMAT of the plane textures; R8_UNORM at 8 bits, R16_UNORM above.
    unsigned int planeFormat_ = 0;   // DXGI_FORMAT_UNKNOWN
    // Which shader and which resources the next draw should bind.
    bool contentIsPlanar_ = false;

    int textureWidth_ = 0;
    int textureHeight_ = 0;
    // Size of the frame currently in the texture, which is what the aspect fit
    // is computed from. Distinct from the texture size only in that the texture
    // can outlive a frame.
    QSize contentSize_;
    bool hasContent_ = false;
    // What is in the texture is the placeholder, not a frame: the fit is then
    // 1:1 with the window rather than aspect-preserved.
    bool contentIsPlaceholder_ = false;
    QSize placeholderSize_;
    QString placeholderText_ = QStringLiteral("Drop media or File > Open");
    bool placeholderDirty_ = true;

    QSize swapChainSize_;
    // Reports "d3d11" or "d3d11 (warp)". The distinction matters on CI, which
    // has no GPU, and in any report where the numbers look wrong for hardware.
    QString name_ = QStringLiteral("d3d11");
    RenderStats stats_{};
};

} // namespace trace::render
