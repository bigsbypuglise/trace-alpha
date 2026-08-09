#pragma once

#include <QImage>
#include <QSize>
#include <QString>

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
    // The empty state. Rendered on the CPU into an image and uploaded through
    // the same path as a frame, so the placeholder survives the move to a
    // native surface instead of quietly becoming a black rectangle.
    void uploadPlaceholder(QSize pixelSize);
    void releaseSizeDependent();

    // The window the swapchain owns. A child of the host widget's HWND rather
    // than the HWND itself.
    HWND surface_ = nullptr;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;
    ComPtr<ID3D11RenderTargetView> backBufferRtv_;
    ComPtr<ID3D11VertexShader> vertexShader_;
    ComPtr<ID3D11PixelShader> pixelShader_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> rasterizer_;
    ComPtr<ID3D11Texture2D> texture_;
    ComPtr<ID3D11ShaderResourceView> textureSrv_;

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
