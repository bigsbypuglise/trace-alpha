#include "render/D3D11VideoRenderer.h"

#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QPainter>
#include <QWidget>
#include <windowsx.h>

#include <algorithm>
#include <cstring>

// Compiled by fxc at build time, not by D3DCompile at runtime: d3dcompiler_47
// is not deployed by windeployqt, so a runtime compile would configure green
// and then fail to create a shader on a clean machine -- exactly the failure
// the "green must mean launchable" rule exists to prevent.
#include "FullscreenQuad.vs.h"
#include "TexturedQuad.ps.h"

namespace trace::render {
namespace {

constexpr float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
// Developer-only. Clears the back buffer to red instead of black, which
// separates the two things a black video rect can mean: "nothing is being
// presented" from "black pixels are being presented correctly".
//
// This is the diagnostic that located the GATE B fault in a single run. The red
// appeared with a black strip exactly the size and position of the video
// viewport, which cleared present, compositing, letterboxing and viewport
// arithmetic all at once and moved the search to the texture -- where the
// source frame turned out to be genuinely black. Kept because the ambiguity it
// resolves is permanent: black is both a legitimate picture and the colour of
// failure. See scripts/measure/visual.ps1, which asserts on it.
constexpr float kDiagRed[4] = {1.0f, 0.0f, 0.0f, 1.0f};

bool clearDiagnosticEnabled() {
    static const bool on = !qgetenv("TRACE_D3D11_CLEAR_DIAG").isEmpty();
    return on;
}

QString hrText(const char* what, HRESULT hr) {
    return QStringLiteral("%1 failed (hr 0x%2)")
        .arg(QString::fromLatin1(what))
        .arg(static_cast<quint32>(hr), 8, 16, QLatin1Char('0'));
}

const wchar_t* kSurfaceClass = L"TraceD3D11Surface";

bool registerSurfaceClass() {
    static const bool registered = [] {
        WNDCLASSEXW wc = {};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = D3D11VideoRenderer::surfaceProcThunk;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;  // never erased; see WM_ERASEBKGND
        wc.lpszClassName = kSurfaceClass;
        return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
    }();
    return registered;
}

} // namespace


// Recovers the instance and forwards. Set on the window at creation.
LRESULT CALLBACK D3D11VideoRenderer::surfaceProcThunk(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    auto* self = reinterpret_cast<D3D11VideoRenderer*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (self) {
        bool handled = false;
        const LRESULT r = self->handleSurfaceMessage(hwnd, msg, wp, lp, handled);
        if (handled) return r;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// The surface's native input path. This is the supported route for a child
// HWND: the window owns its own hit-testing, and Qt never sees these messages
// because the child is above the widget and takes the hit-test itself (which is
// exactly why plain Qt overlay widgets do not work here -- plan section 18.4).
//
// Coordinates arrive in this window's CLIENT space, which is device pixels of
// the surface -- the same space the overlay lays itself out in. There is no
// conversion, deliberately: one space, no opportunity to get it wrong.
LRESULT D3D11VideoRenderer::handleSurfaceMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp,
                                                 bool& handled) {
    switch (msg) {
        case WM_ERASEBKGND:
            // Every pixel comes from the swapchain. Letting Windows erase would
            // flash the class brush between a resize and the next present.
            handled = true;
            return 1;

        case WM_PAINT: {
            // Validate the region without drawing. Presents are driven by Qt's
            // paintEvent, so an unvalidated WM_PAINT would be re-posted forever
            // and spin the message loop.
            PAINTSTRUCT ps;
            BeginPaint(hwnd, &ps);
            EndPaint(hwnd, &ps);
            handled = true;
            return 0;
        }

        case WM_MOUSEACTIVATE:
            // Do not take activation. Keyboard belongs to the Qt window --
            // stepping, J-K-L, Escape and the fullscreen shortcut all live
            // there, and a click on the video must not silently move focus out
            // of it. This is what makes "keyboard focus returns" true by
            // construction rather than by restoring it afterwards.
            handled = true;
            return MA_NOACTIVATE;

        case WM_MOUSEMOVE: {
            if (!mouseTracking_) {
                // One-shot: Windows only sends WM_MOUSELEAVE if asked, and it
                // must be re-armed after every leave.
                TRACKMOUSEEVENT tme = {};
                tme.cbSize = sizeof(tme);
                tme.dwFlags = TME_LEAVE;
                tme.hwndTrack = hwnd;
                TrackMouseEvent(&tme);
                mouseTracking_ = true;
            }
            overlay_.onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            handled = true;
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            overlay_.onMouseLeave();
            handled = true;
            return 0;

        case WM_LBUTTONDOWN:
            // Capture so a drag that leaves the window still delivers moves and
            // the release, which is what makes a timeline drag survive the
            // pointer running off the panel.
            SetCapture(hwnd);
            overlay_.onMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            handled = true;
            return 0;

        case WM_LBUTTONUP:
            ReleaseCapture();
            overlay_.onMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            handled = true;
            return 0;

        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            // Never activated, so this should not arrive -- but if it ever
            // does, hand it to the Qt window rather than swallowing it, so
            // Escape and the fullscreen shortcut keep working.
            if (host_) {
                PostMessageW(reinterpret_cast<HWND>(host_->window()->winId()), msg, wp, lp);
                handled = true;
                return 0;
            }
            break;

        default:
            break;
    }
    return 0;
}

D3D11VideoRenderer::~D3D11VideoRenderer() {
    // The context can hold references to the views being destroyed; clearing it
    // first means the swapchain is not released while still bound.
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
    if (surface_) {
        // Before DestroyWindow: a message delivered during teardown must not
        // find a half-destroyed object through GWLP_USERDATA.
        SetWindowLongPtrW(surface_, GWLP_USERDATA, 0);
        DestroyWindow(surface_);
        surface_ = nullptr;
    }
}

// The swapchain gets its own child window rather than the host widget's HWND.
//
// BOTH APPROACHES WERE MEASURED AND BOTH WORK. The other one -- WA_PaintOnScreen
// plus a null paintEngine(), presenting straight into the widget's own HWND --
// was built first, appeared to show a black video rect, and was replaced on the
// theory that Qt's backing store was painting over it. That theory was wrong.
// The clip under test simply opens on a black frame, and every capture of it was
// a correct render of black. Re-run afterwards on a clip with a non-black first
// frame, the host-HWND approach presents perfectly.
//
// So the choice is on other grounds, and they are narrow but real:
//
//   - WA_PaintOnScreen is documented as X11-only. It happens to work on Windows;
//     nothing says it will keep doing so, and GATE E adds waitable swapchains on
//     top of whatever this is built on.
//   - Child-window compositing above the parent's client area, plus
//     WS_CLIPCHILDREN to exclude the region from the parent's own painting, is
//     documented Win32 behaviour.
//   - Qt's widget model is left completely alone. ViewerWidget needs no
//     paintEngine() override, so nothing about the app changes shape when the
//     GPU backend is switched off.
//
// It also answers hazard 1 in plan section 3 for free: this window is not
// registered as an OLE drop target, so a drop over the video area falls through
// to the ancestor that is -- the one Qt registered for MainWindow. Drag and drop
// keeps working without Trace forwarding anything.
bool D3D11VideoRenderer::createSurfaceWindow(void* parentHwnd, QSize pixelSize, QString& error) {
    if (!registerSurfaceClass()) {
        error = QStringLiteral("RegisterClassEx failed (%1)").arg(GetLastError());
        return false;
    }

    auto parent = static_cast<HWND>(parentHwnd);
    // Without this the parent's paint covers the child until the child's next
    // present, which reads as flicker on every resize.
    SetWindowLongPtrW(parent, GWL_STYLE,
                      GetWindowLongPtrW(parent, GWL_STYLE) | WS_CLIPCHILDREN);

    surface_ = CreateWindowExW(0, kSurfaceClass, nullptr,
                               WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS,
                               0, 0, std::max(1, pixelSize.width()),
                               std::max(1, pixelSize.height()),
                               parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!surface_) {
        error = QStringLiteral("CreateWindowEx failed (%1)").arg(GetLastError());
        return false;
    }
    SetWindowLongPtrW(surface_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

bool D3D11VideoRenderer::createDevice(QString& error) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifndef NDEBUG
    // Only if the SDK layers are actually installed; the retry below covers the
    // common case where they are not.
    flags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    // 10.0 is the floor: the shaders are vs_4_0/ps_4_0 and nothing here needs
    // more. It also keeps WARP and older integrated parts in scope.
    static const D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0,
    };

    const auto tryCreate = [&](D3D_DRIVER_TYPE type, UINT createFlags) {
        return D3D11CreateDevice(nullptr, type, nullptr, createFlags, levels,
                                 static_cast<UINT>(std::size(levels)),
                                 D3D11_SDK_VERSION, &device_, nullptr, &context_);
    };

    HRESULT hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, flags);
#ifndef NDEBUG
    if (FAILED(hr)) {
        flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = tryCreate(D3D_DRIVER_TYPE_HARDWARE, flags);
    }
#endif
    if (FAILED(hr)) {
        // CI runners have no GPU. WARP is a real, correct rasteriser -- slow,
        // but it makes "the D3D11 path was exercised" a thing CI can assert.
        // Named in the HUD so a machine that quietly fell back to software is
        // never mistaken for one measuring hardware.
        hr = tryCreate(D3D_DRIVER_TYPE_WARP, flags);
        if (SUCCEEDED(hr)) name_ = QStringLiteral("d3d11 (warp)");
    }
    if (FAILED(hr)) {
        error = hrText("D3D11CreateDevice", hr);
        return false;
    }
    return true;
}

bool D3D11VideoRenderer::createSwapChain(void* hwnd, QSize pixelSize, QString& error) {
    ComPtr<IDXGIDevice> dxgiDevice;
    HRESULT hr = device_.As(&dxgiDevice);
    if (FAILED(hr)) { error = hrText("QueryInterface(IDXGIDevice)", hr); return false; }

    ComPtr<IDXGIAdapter> adapter;
    hr = dxgiDevice->GetAdapter(&adapter);
    if (FAILED(hr)) { error = hrText("IDXGIDevice::GetAdapter", hr); return false; }

    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { error = hrText("IDXGIAdapter::GetParent(IDXGIFactory2)", hr); return false; }

    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width = static_cast<UINT>(std::max(1, pixelSize.width()));
    desc.Height = static_cast<UINT>(std::max(1, pixelSize.height()));
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    // Flip model, and two buffers. This is the mode that later supports a
    // waitable swapchain and DXGI frame-latency control, which is the whole
    // reason the plan chose native D3D11 over QRhi -- picking BitBlt here would
    // build the GATE E dead end in on day one.
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;

    hr = factory->CreateSwapChainForHwnd(device_.Get(), static_cast<HWND>(hwnd),
                                         &desc, nullptr, nullptr, &swapChain_);
    if (FAILED(hr)) { error = hrText("CreateSwapChainForHwnd", hr); return false; }

    // Alt+Enter belongs to the application. DXGI's default handler would put
    // the swapchain into exclusive fullscreen behind Qt's back, and Trace has
    // its own fullscreen (a window state change that keeps the HUD and
    // transport visible).
    factory->MakeWindowAssociation(static_cast<HWND>(hwnd), DXGI_MWA_NO_ALT_ENTER);

    swapChainSize_ = pixelSize;
    return true;
}

bool D3D11VideoRenderer::createPipeline(QString& error) {
    HRESULT hr = device_->CreateVertexShader(g_TraceFullscreenQuadVS,
                                             sizeof(g_TraceFullscreenQuadVS),
                                             nullptr, &vertexShader_);
    if (FAILED(hr)) { error = hrText("CreateVertexShader", hr); return false; }

    hr = device_->CreatePixelShader(g_TraceTexturedQuadPS, sizeof(g_TraceTexturedQuadPS),
                                    nullptr, &pixelShader_);
    if (FAILED(hr)) { error = hrText("CreatePixelShader", hr); return false; }

    // Bilinear, matching what the CPU backend does when it resamples. GATE B is
    // judged against that backend's picture, so the filter has to be the same
    // kind of filter -- a sharper one here would look like a GPU win that was
    // really a change of resampler.
    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    hr = device_->CreateSamplerState(&sd, &sampler_);
    if (FAILED(hr)) { error = hrText("CreateSamplerState", hr); return false; }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    // The fullscreen triangle has no meaningful winding, and culling it by
    // accident is a black screen with no error anywhere.
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    hr = device_->CreateRasterizerState(&rd, &rasterizer_);
    if (FAILED(hr)) { error = hrText("CreateRasterizerState", hr); return false; }

    return true;
}

bool D3D11VideoRenderer::initialize(QWidget* host, QString& error) {
    if (!host) { error = QStringLiteral("no host widget"); return false; }

    // Device pixels, not logical: on a scaled display the HWND is that many
    // pixels across, and sizing the swapchain in logical units would present a
    // soft, undersized picture stretched back up by the compositor.
    const double dpr = host->devicePixelRatioF();
    const QSize pixels(std::max(1, static_cast<int>(host->width() * dpr)),
                       std::max(1, static_cast<int>(host->height() * dpr)));

    if (!createDevice(error)) return false;
    // winId() realises the native window; ViewerWidget has set WA_NativeWindow
    // so this is the widget's own HWND, and the surface becomes a child of it.
    // TEMPORARY SPIKE KNOB: present into the host widget's own HWND instead of
    // a child window, to test that variant's overlay compatibility. Removed
    // once the surface design is settled.
    if (qgetenv("TRACE_D3D11_HOSTHWND").isEmpty()) {
        if (!createSurfaceWindow(reinterpret_cast<void*>(host->winId()), pixels, error)) return false;
        if (!createSwapChain(surface_, pixels, error)) return false;
    } else {
        if (!createSwapChain(reinterpret_cast<void*>(host->winId()), pixels, error)) return false;
    }
    if (!createPipeline(error)) return false;
    if (!ensureRenderTarget(error)) return false;

    host_ = host;
    QString overlayError;
    if (!overlay_.initialize(device_.Get(), context_.Get(), overlayError)) {
        // Not fatal: the video path is what this backend is for, and an overlay
        // that cannot build must not cost the user their picture.
        qWarning().noquote() << "Trace: overlay compositor disabled:" << overlayError;
    } else {
        const bool overlayOn = !qgetenv("TRACE_OVERLAY_COMPOSITED").isEmpty();
        overlay_.setEnabled(overlayOn);
        if (overlayOn) {
            // Say so on stderr. This is a disposable architecture spike with
            // placeholder art, and anyone who switches it on should be in no
            // doubt that it is not a feature.
            qWarning().noquote()
                << "Trace: EXPERIMENTAL composited overlay enabled "
                   "(spike, placeholder art, not the final interface).";
        }
    }

    return true;
}

void D3D11VideoRenderer::setOverlayHooks(const OverlayHooks& hooks) {
    overlay_.setHooks(hooks);
}

void D3D11VideoRenderer::releaseSizeDependent() {
    // Every reference to the back buffer must be gone before ResizeBuffers, and
    // the context holds one until it is told otherwise.
    backBufferRtv_.Reset();
    if (context_) {
        ID3D11RenderTargetView* none[] = {nullptr};
        context_->OMSetRenderTargets(1, none, nullptr);
        context_->Flush();
    }
}

bool D3D11VideoRenderer::ensureRenderTarget(QString& error) {
    if (backBufferRtv_) return true;
    if (!swapChain_) { error = QStringLiteral("no swapchain"); return false; }

    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr)) { error = hrText("IDXGISwapChain::GetBuffer", hr); return false; }

    hr = device_->CreateRenderTargetView(backBuffer.Get(), nullptr, &backBufferRtv_);
    if (FAILED(hr)) { error = hrText("CreateRenderTargetView", hr); return false; }
    return true;
}

bool D3D11VideoRenderer::ensureTexture(int width, int height) {
    if (texture_ && textureWidth_ == width && textureHeight_ == height) return true;

    texture_.Reset();
    textureSrv_.Reset();
    textureWidth_ = 0;
    textureHeight_ = 0;

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = static_cast<UINT>(width);
    td.Height = static_cast<UINT>(height);
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    // DYNAMIC + WRITE_DISCARD: the frame changes every present and the previous
    // contents are never read back, so discarding lets the driver hand back a
    // fresh region instead of stalling on the one the GPU may still be reading.
    td.Usage = D3D11_USAGE_DYNAMIC;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;

    if (FAILED(device_->CreateTexture2D(&td, nullptr, &texture_))) return false;
    if (FAILED(device_->CreateShaderResourceView(texture_.Get(), nullptr, &textureSrv_))) {
        texture_.Reset();
        return false;
    }

    textureWidth_ = width;
    textureHeight_ = height;
    return true;
}

void D3D11VideoRenderer::uploadPixels(const uint8_t* src, int srcStride, int width, int height) {
    if (!context_ || !texture_ || !src) return;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(texture_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;

    // The two strides are independent: the frame's comes from swscale (padded
    // to QImage's rule) and the mapped one from the driver. Copying row by row
    // is what makes this correct for both rather than for whichever happens to
    // match today -- and a whole-buffer memcpy that assumed they matched would
    // shear the picture on any machine where they do not.
    const int rowBytes = width * 4;
    auto* dst = static_cast<uint8_t*>(mapped.pData);
    if (srcStride == static_cast<int>(mapped.RowPitch) && srcStride == rowBytes) {
        std::memcpy(dst, src, static_cast<size_t>(rowBytes) * height);
    } else {
        for (int y = 0; y < height; ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
                        src + static_cast<size_t>(y) * srcStride,
                        static_cast<size_t>(rowBytes));
        }
    }

    context_->Unmap(texture_.Get(), 0);
}

void D3D11VideoRenderer::setFrame(const trace::core::VideoFrame& frame) {
    if (frame.isNull() || !device_) { clearFrame(); return; }

    const auto& buffer = *frame.buffer;
    if (buffer.layout() != trace::core::PixelLayout::BGRA8) {
        // Nothing else can reach here today -- swscale's destination is BGRA
        // and the still path adopts an RGB32 QImage -- but the check is the
        // difference between an unsupported layout being a black screen and
        // being a diagnosable one.
        clearFrame();
        return;
    }

    if (!ensureTexture(buffer.width(), buffer.height())) { clearFrame(); return; }
    uploadPixels(buffer.data(), buffer.bytesPerLine(), buffer.width(), buffer.height());

    contentSize_ = QSize(buffer.width(), buffer.height());
    hasContent_ = true;
    contentIsPlaceholder_ = false;
}

void D3D11VideoRenderer::clearFrame() {
    hasContent_ = false;
    contentIsPlaceholder_ = false;
    contentSize_ = QSize();
    // The placeholder is what gets shown instead, and it has to be rebuilt
    // because the texture now holds a frame.
    placeholderDirty_ = true;
}

void D3D11VideoRenderer::setPlaceholderText(const QString& text) {
    if (placeholderText_ == text) return;
    placeholderText_ = text;
    placeholderDirty_ = true;
}

void D3D11VideoRenderer::uploadPlaceholder(QSize pixelSize) {
    if (pixelSize.isEmpty()) return;

    QImage image(pixelSize, QImage::Format_RGB32);
    image.fill(QColor(0, 0, 0));
    {
        QPainter p(&image);
        p.setPen(QColor(150, 150, 150));
        p.drawText(QRect(QPoint(0, 0), pixelSize), Qt::AlignCenter, placeholderText_);
    }

    if (!ensureTexture(pixelSize.width(), pixelSize.height())) return;
    uploadPixels(image.constBits(), static_cast<int>(image.bytesPerLine()),
                 pixelSize.width(), pixelSize.height());

    placeholderSize_ = pixelSize;
    placeholderDirty_ = false;
    contentIsPlaceholder_ = true;
    contentSize_ = pixelSize;
}

void D3D11VideoRenderer::resize(QSize size) {
    Q_UNUSED(size);
    // The real size is read from the host at paint time, in device pixels.
    // Marking the target stale here and rebuilding there means one path handles
    // both an explicit resize and a DPI change, which does not come through
    // resizeEvent at all.
    releaseSizeDependent();
    // A resized window needs the placeholder re-rendered at the new size; a
    // frame does not, because it is fitted to the window rather than drawn at
    // window size.
    if (!hasContent_) placeholderDirty_ = true;
}

void D3D11VideoRenderer::paint(QWidget* host) {
    if (!host || !swapChain_ || !context_) return;

    QElapsedTimer timer;
    timer.start();
    double drawMs = 0.0;

    const double dpr = host->devicePixelRatioF();
    const QSize pixels(std::max(1, static_cast<int>(host->width() * dpr)),
                       std::max(1, static_cast<int>(host->height() * dpr)));

    if (pixels != swapChainSize_) {
        // The surface window tracks the widget. Done here rather than in
        // resize() so a DPI change -- which does not arrive as a resizeEvent --
        // is handled by the same path.
        if (surface_) {
            SetWindowPos(surface_, nullptr, 0, 0, pixels.width(), pixels.height(),
                         SWP_NOZORDER | SWP_NOACTIVATE);
        }
        releaseSizeDependent();
        // Same format and buffer count; 0 flags. A failure here leaves the old
        // buffers intact, so the next paint retries rather than presenting
        // into nothing.
        if (SUCCEEDED(swapChain_->ResizeBuffers(0, static_cast<UINT>(pixels.width()),
                                                static_cast<UINT>(pixels.height()),
                                                DXGI_FORMAT_UNKNOWN, 0))) {
            swapChainSize_ = pixels;
            if (!hasContent_) placeholderDirty_ = true;
        }
    }

    QString error;
    if (!ensureRenderTarget(error)) return;

    if (!hasContent_ && (placeholderDirty_ || placeholderSize_ != pixels)) {
        uploadPlaceholder(pixels);
    }

    // Letterbox by viewport. Everything outside it is the clear colour, so the
    // bars come for free and the shader never has to know about them.
    QSize fitted = pixels;
    bool resampled = false;
    if (hasContent_ && !contentSize_.isEmpty()) {
        fitted = contentSize_.scaled(pixels, Qt::KeepAspectRatio);
        resampled = (fitted != contentSize_);
    }

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = static_cast<float>((pixels.width() - fitted.width()) / 2);
    vp.TopLeftY = static_cast<float>((pixels.height() - fitted.height()) / 2);
    vp.Width = static_cast<float>(std::max(1, fitted.width()));
    vp.Height = static_cast<float>(std::max(1, fitted.height()));
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtvs[] = {backBufferRtv_.Get()};
    context_->OMSetRenderTargets(1, rtvs, nullptr);
    context_->ClearRenderTargetView(backBufferRtv_.Get(),
                                    clearDiagnosticEnabled() ? kDiagRed : kBlack);

    if (textureSrv_) {
        QElapsedTimer drawTimer;
        drawTimer.start();

        context_->RSSetViewports(1, &vp);
        context_->RSSetState(rasterizer_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // No input layout and no vertex buffer: the vertex shader builds the
        // triangle from SV_VertexID.
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
        ID3D11ShaderResourceView* srvs[] = {textureSrv_.Get()};
        context_->PSSetShaderResources(0, 1, srvs);
        ID3D11SamplerState* samplers[] = {sampler_.Get()};
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);

        drawMs = static_cast<double>(drawTimer.nsecsElapsed()) / 1'000'000.0;
    }

    // After the video, before Present. Its own viewport and blend state; it
    // restores neither, because every path into paint() sets both.
    overlay_.setDevicePixelRatio(dpr);
    overlay_.draw(pixels);

    stats_.lastDrawWasScaled = resampled;
    // Always filtered when resampled: unlike the CPU path there is no
    // point-sampled variant to A/B against, and the sampler is set once.
    stats_.lastDrawWasFiltered = resampled;
    stats_.lastDrawSize = fitted;

    // Same scope split as the CPU backend: paintMs is the body, paintTotalMs
    // includes the present. Present is the analogue of ~QPainter's flush, so
    // the two backends' numbers stay comparable.
    const double paintMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    stats_.lastPaintMs = paintMs;
    ++stats_.samples;
    stats_.avgPaintMs += (paintMs - stats_.avgPaintMs) / static_cast<double>(stats_.samples);

    // Sync interval 0: presentation pacing is GATE E's problem, and pacing here
    // would silently become the frame scheduler while the audio clock still
    // believes it is one. See plan section 9 -- vsync becomes the phase
    // authority there, deliberately and with the composition rule stated.
    swapChain_->Present(0, 0);

    const double totalMs = static_cast<double>(timer.nsecsElapsed()) / 1'000'000.0;
    stats_.lastPaintTotalMs = totalMs;
    stats_.lastDrawImageMs = drawMs;
    ++stats_.paintCount;
    const double pn = static_cast<double>(stats_.paintCount);
    stats_.avgPaintTotalMs += (totalMs - stats_.avgPaintTotalMs) / pn;
    stats_.avgDrawImageMs += (drawMs - stats_.avgDrawImageMs) / pn;
}

} // namespace trace::render
