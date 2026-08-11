#include "render/D3D11VideoRenderer.h"

#include <QColor>
#include <QDebug>
#include <QElapsedTimer>
#include <QPainter>
#include <QWidget>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

// Compiled by fxc at build time, not by D3DCompile at runtime: d3dcompiler_47
// is not deployed by windeployqt, so a runtime compile would configure green
// and then fail to create a shader on a clean machine -- exactly the failure
// the "green must mean launchable" rule exists to prevent.
#include "FullscreenQuad.vs.h"
#include "TexturedQuad.ps.h"
#include "YuvToRgb.ps.h"

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

// Step 9's filtered reduction. On by default; TRACE_GPU_REDUCE=0 restores the
// single bilinear tap.
//
// It has to be separately switchable from TRACE_PLANAR_UPLOAD, and that is not a
// convenience. The reduction lives in the YUV shader only, so with it on, the
// planar and BGRA paths filter differently -- and TRACE_PLANAR_UPLOAD=0 is the
// control for every planar measurement. Without this knob that control would
// differ in two ways at once, which is the failure section 22.4a is a whole
// section about.
bool reductionEnabled() {
    static const bool on = qgetenv("TRACE_GPU_REDUCE") != QByteArray("0");
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
        // CS_DBLCLKS or WM_LBUTTONDBLCLK is NEVER SENT -- Windows delivers a
        // second WM_LBUTTONDOWN instead, silently, and double-click-to-fullscreen
        // would look like it had simply not been wired up. A class style, so it
        // has to be right at registration; there is no per-window fix later.
        wc.style = CS_DBLCLKS;
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
            if (overlayModel_) overlayModel_->onMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            handled = true;
            return 0;
        }

        case WM_MOUSELEAVE:
            mouseTracking_ = false;
            if (overlayModel_) overlayModel_->onMouseLeave();
            handled = true;
            return 0;

        case WM_LBUTTONDOWN:
            // Capture so a drag that leaves the window still delivers moves and
            // the release, which is what makes a timeline drag survive the
            // pointer running off the panel.
            SetCapture(hwnd);
            if (overlayModel_) overlayModel_->onMouseDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            handled = true;
            return 0;

        case WM_LBUTTONUP:
            ReleaseCapture();
            if (overlayModel_) overlayModel_->onMouseUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            handled = true;
            return 0;

        case WM_LBUTTONDBLCLK:
            // Only arrives because the window class carries CS_DBLCLKS. Note the
            // sequence Windows sends is down, up, DBLCLK, up -- so the overlay
            // has already seen a complete click on whatever is under the pointer,
            // which is why the model treats a double-click on a control as that
            // control being used rather than as a window gesture.
            if (overlayModel_) {
                overlayModel_->onMouseDoubleClick(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
            }
            handled = true;
            return 0;

        case WM_SETCURSOR:
            // Only claim the message for the client area; the surface is a child
            // with no frame, but answering unconditionally is the habit that
            // breaks resize cursors the day it grows one.
            if (cursorHidden_ && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                handled = true;
                return TRUE;
            }
            break;

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

    // The view transform's 2x2, at the vertex stage. Fatal if it fails, unlike
    // the YUV buffer below: the vertex shader reads it unconditionally, so a
    // missing buffer is not a lost capability but an undefined texture
    // coordinate on every frame.
    {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 16;   // one float4
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = device_->CreateBuffer(&bd, nullptr, &viewParams_);
        if (FAILED(hr)) { error = hrText("CreateBuffer(ViewParams)", hr); return false; }
        if (!uploadViewParams()) {
            error = QStringLiteral("failed to seed the view-transform constants");
            return false;
        }
    }

    // GATE C. A failure here is NOT fatal: without the YUV shader the backend
    // still presents every BGRA frame exactly as it did at GATE B, and the
    // decoder is told not to send planes because acceptsPlanarYuv() is answered
    // from whether this exists. Refusing to initialize at all would turn a
    // missing capability into a fallback to the CPU renderer.
    hr = device_->CreatePixelShader(g_TraceYuvToRgbPS, sizeof(g_TraceYuvToRgbPS),
                                    nullptr, &yuvPixelShader_);
    if (FAILED(hr)) {
        yuvPixelShader_.Reset();
        qWarning().noquote() << "Trace: YUV pixel shader unavailable"
                             << hrText("CreatePixelShader(YuvToRgb)", hr)
                             << "- planar upload disabled, BGRA path unchanged.";
    } else {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = 80;   // three float4 rows plus five floats and padding
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateBuffer(&bd, nullptr, &yuvParams_))) {
            yuvParams_.Reset();
            yuvPixelShader_.Reset();
            qWarning().noquote() << "Trace: YUV constant buffer failed"
                                 << "- planar upload disabled, BGRA path unchanged.";
        }
    }

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
    // Built whether or not the overlay is switched on: it allocates a pipeline
    // and no per-frame work, and building it here rather than on first reveal
    // keeps a shader compile out of the presentation path.
    QString overlayError;
    if (!overlay_.initialize(device_.Get(), context_.Get(), overlayError)) {
        // Not fatal: the video path is what this backend is for, and an overlay
        // that cannot build must not cost the user their picture. The host is
        // told by the absence of a drawn overlay, not by a failed initialize.
        qWarning().noquote() << "Trace: overlay drawer disabled:" << overlayError;
        overlayFailed_ = true;
    }

    return true;
}

void D3D11VideoRenderer::setOverlay(OverlayModel* model) {
    overlayModel_ = overlayFailed_ ? nullptr : model;
}

void D3D11VideoRenderer::setCursorHidden(bool hidden) {
    if (cursorHidden_ == hidden) return;
    cursorHidden_ = hidden;
    // WM_SETCURSOR only arrives on pointer movement, so the change has to be
    // applied now as well as answered later: hiding it happens precisely when
    // the pointer has STOPPED moving, and waiting for the next message would
    // mean the cursor reappears at the moment it is meant to vanish.
    if (surface_ && surface_ == GetCapture()) {
        SetCursor(hidden ? nullptr : LoadCursor(nullptr, IDC_ARROW));
    } else if (surface_) {
        POINT pt{};
        if (GetCursorPos(&pt) && WindowFromPoint(pt) == surface_) {
            SetCursor(hidden ? nullptr : LoadCursor(nullptr, IDC_ARROW));
        }
    }
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
    ++stats_.textureCreates;
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

void D3D11VideoRenderer::releasePlaneTextures() {
    for (int i = 0; i < 3; ++i) {
        planeTexture_[i].Reset();
        planeSrv_[i].Reset();
        planeWidth_[i] = 0;
        planeHeight_[i] = 0;
    }
    planeFormat_ = DXGI_FORMAT_UNKNOWN;
}

bool D3D11VideoRenderer::ensurePlaneTextures(const trace::core::FrameBuffer& buffer) {
    // R8 at 8 bits, R16 above. R16_UNORM normalises by 65535 while the samples
    // occupy only the low 10 or 12 bits, which is what the shader's sampleScale
    // corrects; storing them this way is what makes the CPU-side copy a memcpy.
    const DXGI_FORMAT want =
        buffer.bytesPerSample() == 2 ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;

    bool ok = planeFormat_ == static_cast<unsigned int>(want);
    if (ok) {
        for (int i = 0; i < 3 && ok; ++i) {
            ok = planeTexture_[i] && planeWidth_[i] == buffer.planeWidth(i)
                 && planeHeight_[i] == buffer.planeHeight(i);
        }
    }
    if (ok) return true;

    releasePlaneTextures();

    for (int i = 0; i < 3; ++i) {
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(buffer.planeWidth(i));
        td.Height = static_cast<UINT>(buffer.planeHeight(i));
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = want;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (td.Width == 0 || td.Height == 0) { releasePlaneTextures(); return false; }
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &planeTexture_[i]))) {
            releasePlaneTextures();
            return false;
        }
        ++stats_.textureCreates;
        if (FAILED(device_->CreateShaderResourceView(planeTexture_[i].Get(), nullptr,
                                                     &planeSrv_[i]))) {
            releasePlaneTextures();
            return false;
        }
        planeWidth_[i] = buffer.planeWidth(i);
        planeHeight_[i] = buffer.planeHeight(i);
    }
    planeFormat_ = static_cast<unsigned int>(want);
    return true;
}

bool D3D11VideoRenderer::uploadPlanes(const trace::core::FrameBuffer& buffer) {
    if (!context_) return false;
    const int sampleBytes = buffer.bytesPerSample();
    for (int i = 0; i < 3; ++i) {
        const uint8_t* src = buffer.data(i);
        if (!src || !planeTexture_[i]) return false;

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(context_->Map(planeTexture_[i].Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                                 &mapped))) {
            return false;
        }
        // Row by row for the same reason as the BGRA path: the source stride is
        // ours and the destination stride is the driver's, and they are not the
        // same number.
        const int rowBytes = buffer.planeWidth(i) * sampleBytes;
        const int srcStride = buffer.bytesPerLine(i);
        auto* dst = static_cast<uint8_t*>(mapped.pData);
        for (int y = 0; y < buffer.planeHeight(i); ++y) {
            std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
                        src + static_cast<size_t>(y) * srcStride,
                        static_cast<size_t>(rowBytes));
        }
        context_->Unmap(planeTexture_[i].Get(), 0);
    }
    return true;
}

bool D3D11VideoRenderer::updateYuvParams(const trace::core::VideoFrame& frame) {
    if (!context_ || !yuvParams_ || !frame.buffer) return false;

    // Rows of the YUV->RGB matrix, from plan section 11. The vector is
    // (y, Cb, Cr) with the chroma already centred at zero by the offsets below.
    float r[3], g[3], b[3];
    using M = trace::core::ColorInfo::Matrix;
    switch (frame.color.matrix) {
        case M::BT709:
            r[0] = 1.0f; r[1] =  0.0f;      r[2] =  1.5748f;
            g[0] = 1.0f; g[1] = -0.1873f;   g[2] = -0.4681f;
            b[0] = 1.0f; b[1] =  1.8556f;   b[2] =  0.0f;
            break;
        case M::BT601:
            r[0] = 1.0f; r[1] =  0.0f;      r[2] =  1.4020f;
            g[0] = 1.0f; g[1] = -0.344136f; g[2] = -0.714136f;
            b[0] = 1.0f; b[1] =  1.7720f;   b[2] =  0.0f;
            break;
        case M::BT2020:
            r[0] = 1.0f; r[1] =  0.0f;      r[2] =  1.4746f;
            g[0] = 1.0f; g[1] = -0.16455f;  g[2] = -0.57135f;
            b[0] = 1.0f; b[1] =  1.8814f;   b[2] =  0.0f;
            break;
        default:
            // Fcc, Smpte240m and Unspecified have no entry here on purpose.
            // Presenting them through a near-enough matrix would put a colour
            // difference between the two backends that no A/B could attribute.
            // The decoder declines the same set, so this is a backstop.
            return false;
    }

    const int depth = frame.buffer->bitDepth();
    const float maxCode = static_cast<float>((1 << depth) - 1);
    const int shift = depth - 8;

    // Computed at the ACTUAL depth rather than reusing the 8-bit fractions:
    // 10-bit black is code 64 of 1023, which is not 16/255. See the shader.
    const float blackCode  = static_cast<float>(16 << shift);
    const float whiteCode  = static_cast<float>(235 << shift);
    const float chromaMid  = static_cast<float>(128 << shift);
    const float chromaSpan = static_cast<float>(224 << shift);

    // The reduction terms already in yuvParamsData_ are deliberately preserved:
    // they belong to the destination rect, which a new frame does not change.
    YuvParamsData& p = yuvParamsData_;

    p.matR[0] = r[0]; p.matR[1] = r[1]; p.matR[2] = r[2];
    p.matG[0] = g[0]; p.matG[1] = g[1]; p.matG[2] = g[2];
    p.matB[0] = b[0]; p.matB[1] = b[1]; p.matB[2] = b[2];

    // R8_UNORM already yields value/255; R16_UNORM yields value/65535 for data
    // that only fills the low `depth` bits.
    p.sampleScale = (frame.buffer->bytesPerSample() == 2) ? (65535.0f / maxCode) : 1.0f;

    if (frame.color.fullRange) {
        p.lumaOffset = 0.0f;
        p.lumaScale = 1.0f;
        p.chromaOffset = chromaMid / maxCode;
        p.chromaScale = 1.0f;
    } else {
        p.lumaOffset = blackCode / maxCode;
        p.lumaScale = maxCode / (whiteCode - blackCode);
        p.chromaOffset = chromaMid / maxCode;
        p.chromaScale = maxCode / chromaSpan;
    }

    return uploadYuvParams();
}

bool D3D11VideoRenderer::uploadYuvParams() {
    if (!context_ || !yuvParams_) return false;
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(yuvParams_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, &yuvParamsData_, sizeof(yuvParamsData_));
    context_->Unmap(yuvParams_.Get(), 0);
    return true;
}

// source_uv = M * (dest_uv - 0.5) + 0.5, so M maps a DESTINATION offset back to
// the source it should be read from -- the inverse of the transform the viewer
// sees. Rotating the picture a quarter turn clockwise therefore means reading
// the source a quarter turn anticlockwise, which is why the signs below look
// like the opposite of what is being asked for.
//
// Derived by corners rather than by trusting a sign convention: under a
// clockwise quarter turn the source's top-left must arrive at the destination's
// TOP-RIGHT, i.e. dest (0,0) reads source (0,1) and dest (1,0) reads source
// (0,0). That gives source.x = dest.y and source.y = 1 - dest.x, which in
// centred coordinates is [[0,1],[-1,0]].
//
// The flip is applied to the DESTINATION coordinate before the rotation, which
// is what makes "flip horizontally" mean flipping what the user can currently
// see rather than flipping the source and then rotating the flip somewhere else.
static void viewMatrix2x2(const ViewTransform& t, float out[4]) {
    // Rotation, clockwise quarter turns, in centred uv space (y down).
    float r[4];
    switch (t.quarterTurns) {
        case 1:  r[0] =  0; r[1] =  1; r[2] = -1; r[3] =  0; break;
        case 2:  r[0] = -1; r[1] =  0; r[2] =  0; r[3] = -1; break;
        case 3:  r[0] =  0; r[1] = -1; r[2] =  1; r[3] =  0; break;
        default: r[0] =  1; r[1] =  0; r[2] =  0; r[3] =  1; break;
    }
    const float fx = t.flipH ? -1.0f : 1.0f;
    const float fy = t.flipV ? -1.0f : 1.0f;
    // r * diag(fx, fy): scaling the columns, because the flip is applied first.
    out[0] = r[0] * fx; out[1] = r[1] * fy;
    out[2] = r[2] * fx; out[3] = r[3] * fy;
}

bool D3D11VideoRenderer::uploadViewParams() {
    if (!context_ || !viewParams_) return false;
    float m[4];
    viewMatrix2x2(viewTransform_, m);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(viewParams_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
        return false;
    }
    std::memcpy(mapped.pData, m, sizeof(m));
    context_->Unmap(viewParams_.Get(), 0);
    return true;
}

void D3D11VideoRenderer::setViewTransform(const ViewTransform& transform) {
    if (transform == viewTransform_) return;
    viewTransform_ = transform;
    uploadViewParams();
    // The fit and the reduction both depend on it and are both recomputed in
    // paint(), so there is nothing else to invalidate -- but the picture has
    // changed without a new frame arriving, and only the host can ask for a
    // repaint.
    if (host_) host_->update();
}

// No uploadViewParams() here, unlike setViewTransform: the pixel aspect is not
// in the shader at all. It stretches the DESTINATION rect, and the sampling
// stays in normalised source coordinates, which is what lets one shader keep
// covering every subsampling and bit depth (GATE C) without a variant for
// anamorphic media.
void D3D11VideoRenderer::setPixelAspect(double par) {
    if (par == pixelAspect_) return;
    pixelAspect_ = par;
    if (host_) host_->update();
}

void D3D11VideoRenderer::updateReduction(QSize content, QSize fitted) {
    float taps = 1.0f;
    float fu = 0.0f;
    float fv = 0.0f;

    // Only when reducing. Upscaling has no undersampling to fix -- a bilinear tap
    // is already reading more source detail than the output can hold -- and a box
    // average there would blur a magnified frame, which is the opposite of what a
    // review tool wants when someone is inspecting pixels.
    if (reductionEnabled() && !content.isEmpty() && !fitted.isEmpty()
        && fitted.width() < content.width() && fitted.height() < content.height()) {
        // One destination pixel, in normalised source coordinates.
        fu = 1.0f / static_cast<float>(fitted.width());
        fv = 1.0f / static_cast<float>(fitted.height());

        const double ratio = std::max(
            static_cast<double>(content.width()) / static_cast<double>(fitted.width()),
            static_cast<double>(content.height()) / static_cast<double>(fitted.height()));

        // Each tap is a bilinear 2x2, so N taps reach 2N source texels per axis
        // and ratio/2 taps already cover the footprint. Rounding up rather than
        // down: a footprint left partly unsampled is the defect being fixed.
        //
        // Capped at 4 (16 samples per plane, 48 fetches per output pixel). The cap
        // is not a performance guess about this box -- at 640x360 even 4x4 is ~11M
        // fetches, nothing for any GPU that runs this app. It is there because the
        // backend also has to work on WARP, which is a software rasteriser, and CI
        // renames itself `d3d11 (warp)` and still has to pass. Beyond 4 the
        // remaining error is small and the cost is quadratic.
        const int want = static_cast<int>(std::ceil(ratio / 2.0));
        taps = static_cast<float>(std::clamp(want, 1, 4));
    }

    // `content` arrives ROTATED, so everything above is in the space the picture
    // is displayed in. The shader, however, offsets its taps in SOURCE uv -- so
    // under a quarter turn the destination's horizontal extent is a step along
    // the source's vertical axis, and the two components have to be exchanged
    // on the way into the constant buffer.
    //
    // Getting this wrong is invisible on a square-ish reduction and obvious on
    // an anamorphic one: the box average would filter along the wrong axis,
    // which is step 9's defect reintroduced by a coordinate mistake rather than
    // by a missing loop.
    if (viewTransform_.swapsAxes()) std::swap(fu, fv);

    if (taps == yuvParamsData_.taps
        && fu == yuvParamsData_.footprint[0]
        && fv == yuvParamsData_.footprint[1]) {
        return;
    }
    yuvParamsData_.taps = taps;
    yuvParamsData_.footprint[0] = fu;
    yuvParamsData_.footprint[1] = fv;
    uploadYuvParams();
}

void D3D11VideoRenderer::setFrame(const trace::core::VideoFrame& frame) {
    if (frame.isNull() || !device_) { clearFrame(); return; }

    const auto& buffer = *frame.buffer;

    // Timed from here rather than from inside uploadPlanes/uploadPixels so the
    // number is "what this frame cost to get onto the GPU" including a texture
    // creation on the frames where one happens -- which is the whole question
    // step 8 asks. `textureCreates` says which samples those were, so an
    // inflated one is attributable rather than mysterious.
    //
    // Deliberately only in setFrame: uploadPlaceholder goes through the same two
    // functions but is not a frame, and counting it would put a startup cost in
    // a per-frame average.
    QElapsedTimer uploadTimer;
    uploadTimer.start();
    const auto recordUpload = [this, &uploadTimer]() {
        stats_.lastUploadMs = static_cast<double>(uploadTimer.nsecsElapsed()) / 1e6;
        ++stats_.uploadCount;
        stats_.avgUploadMs += (stats_.lastUploadMs - stats_.avgUploadMs)
                              / static_cast<double>(stats_.uploadCount);
    };

    if (trace::core::isPlanarYuv(buffer.layout())) {
        // Any failure here falls back to nothing rather than to the BGRA path,
        // because there is no BGRA copy of this frame to fall back TO -- the
        // decoder skipped the conversion precisely because this backend said it
        // could take the planes. clearFrame() shows the placeholder, which is
        // diagnosable; a stale previous frame under a new index would not be.
        if (!yuvPixelShader_ || !ensurePlaneTextures(buffer) || !uploadPlanes(buffer)
            || !updateYuvParams(frame)) {
            clearFrame();
            return;
        }
        recordUpload();
        contentSize_ = QSize(buffer.width(), buffer.height());
        hasContent_ = true;
        contentIsPlanar_ = true;
        contentIsPlaceholder_ = false;
        return;
    }

    if (buffer.layout() != trace::core::PixelLayout::BGRA8) {
        // The check is the difference between an unsupported layout being a
        // black screen and being a diagnosable one.
        clearFrame();
        return;
    }

    if (!ensureTexture(buffer.width(), buffer.height())) { clearFrame(); return; }
    uploadPixels(buffer.data(), buffer.bytesPerLine(), buffer.width(), buffer.height());
    recordUpload();

    contentSize_ = QSize(buffer.width(), buffer.height());
    hasContent_ = true;
    contentIsPlanar_ = false;
    contentIsPlaceholder_ = false;
}

void D3D11VideoRenderer::clearFrame() {
    hasContent_ = false;
    contentIsPlaceholder_ = false;
    // The placeholder is BGRA, so the next draw must not bind the YUV shader.
    // The plane textures themselves are kept: clearFrame runs on every media
    // change and between drag and landing, and rebuilding three textures each
    // time would be the allocation this path exists to avoid.
    contentIsPlanar_ = false;
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
    // The placeholder went into the BGRA texture, so the draw must bind the
    // BGRA shader whatever the last frame was.
    contentIsPlanar_ = false;
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
    const QSize pixels = hostDeviceSize(host);

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
    //
    // The fit goes through the shared helper so the CPU backend lands on exactly
    // the same rectangle; they were separate expressions and disagreed by a
    // fraction of a pixel at fractional DPI.
    // The fit is computed from the DISPLAYED size, not the decoded one: a
    // quarter turn exchanges the axes, so a 16:9 source letterboxes as 9:16 and
    // the viewport has to follow it. The placeholder deliberately does not
    // rotate -- it is a message, not the media.
    // Pixel aspect first, then the transform: the SAR describes the stored
    // samples, and a quarter turn exchanges the axes of what those samples
    // become. The other order would apply a horizontal stretch to a picture
    // that had already been turned on its side.
    const QSize displayedSize = viewTransform_.apply(applyPixelAspect(contentSize_, pixelAspect_));
    // The same content in the same on-screen axes but WITHOUT the pixel-aspect
    // stretch, which is what the reduction has to be measured against. See
    // updateReduction: its ratio answers "how many source TEXELS does one
    // destination pixel cover", and the pixel aspect adds no texels -- it only
    // says how wide they are. Feeding it the stretched size would report an
    // anamorphic frame as reducing harder than it is and buy an extra tap per
    // axis, filtering along an axis that has no extra detail on it.
    //
    // Phase 10 hit the neighbouring version of this and the fix was the
    // opposite direction: the taps DO have to follow the rotation. Rotation
    // exchanges real texel axes; the pixel aspect does not.
    const QSize sourceOnScreenAxes = viewTransform_.apply(contentSize_);
    QRect dest(QPoint(0, 0), pixels);
    bool resampled = false;
    if (hasContent_ && !displayedSize.isEmpty()) {
        dest = fitDeviceRect(displayedSize, pixels);
        resampled = (dest.size() != displayedSize);
    }
    const QSize fitted = dest.size();

    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = static_cast<float>(dest.x());
    vp.TopLeftY = static_cast<float>(dest.y());
    vp.Width = static_cast<float>(std::max(1, fitted.width()));
    vp.Height = static_cast<float>(std::max(1, fitted.height()));
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;

    ID3D11RenderTargetView* rtvs[] = {backBufferRtv_.Get()};
    context_->OMSetRenderTargets(1, rtvs, nullptr);
    context_->ClearRenderTargetView(backBufferRtv_.Get(),
                                    clearDiagnosticEnabled() ? kDiagRed : kBlack);

    const bool drawPlanar = contentIsPlanar_ && planeSrv_[0] && planeSrv_[1] && planeSrv_[2]
                            && yuvPixelShader_;
    if (drawPlanar || textureSrv_) {
        QElapsedTimer drawTimer;
        drawTimer.start();

        context_->RSSetViewports(1, &vp);
        context_->RSSetState(rasterizer_.Get());
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        // No input layout and no vertex buffer: the vertex shader builds the
        // triangle from SV_VertexID.
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(vertexShader_.Get(), nullptr, 0);
        // The view transform, at the vertex stage. Bound per draw because the
        // overlay's own pass writes the vertex stage's b0 as well, so leaving it
        // set from last frame would make this draw depend on whether the overlay
        // happened to be visible.
        ID3D11Buffer* viewCbs[] = {viewParams_.Get()};
        context_->VSSetConstantBuffers(0, 1, viewCbs);

        // Both paths share the vertex shader, the sampler, the rasteriser and
        // the viewport. Only the pixel shader and what is bound to it differ,
        // which is exactly the split GATE B's shader comment promised.
        if (drawPlanar) {
            // Before the draw and after the fit, because the ratio is a property
            // of the destination rect. A resize changes it with no new frame.
            updateReduction(sourceOnScreenAxes, fitted);
            context_->PSSetShader(yuvPixelShader_.Get(), nullptr, 0);
            ID3D11ShaderResourceView* srvs[] = {planeSrv_[0].Get(), planeSrv_[1].Get(),
                                                planeSrv_[2].Get()};
            context_->PSSetShaderResources(0, 3, srvs);
            ID3D11Buffer* cbs[] = {yuvParams_.Get()};
            context_->PSSetConstantBuffers(0, 1, cbs);
        } else {
            context_->PSSetShader(pixelShader_.Get(), nullptr, 0);
            // Unbind slots 1 and 2: leaving the chroma planes bound while the
            // BGRA shader runs is harmless today but makes the next frame's
            // state depend on the previous frame's kind.
            ID3D11ShaderResourceView* srvs[] = {textureSrv_.Get(), nullptr, nullptr};
            context_->PSSetShaderResources(0, 3, srvs);
        }
        ID3D11SamplerState* samplers[] = {sampler_.Get()};
        context_->PSSetSamplers(0, 1, samplers);
        context_->Draw(3, 0);

        drawMs = static_cast<double>(drawTimer.nsecsElapsed()) / 1'000'000.0;
    }

    // After the video, before Present. Its own viewport and blend state; it
    // restores neither, because every path into paint() sets both.
    if (overlayModel_) {
        overlayModel_->setDevicePixelRatio(dpr);
        overlay_.draw(*overlayModel_, pixels);
    }

    stats_.lastDrawWasScaled = resampled;
    // Always filtered when resampled: unlike the CPU path there is no
    // point-sampled variant to A/B against, and the sampler is set once.
    stats_.lastDrawWasFiltered = resampled;
    stats_.lastDrawSize = fitted;
    // How many taps per axis the reduction is actually using. Reported for the
    // reason every other capability on this backend is: a filtered reduction that
    // silently stays at 1 looks exactly like a working one until someone measures
    // the pixels. 1 is honest at 1:1 and on the BGRA path.
    stats_.reduceTaps = drawPlanar ? static_cast<int>(yuvParamsData_.taps) : 1;

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
