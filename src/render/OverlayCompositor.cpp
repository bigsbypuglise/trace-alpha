#include "render/OverlayCompositor.h"

#include <QBrush>
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "OverlayQuad.vs.h"
#include "OverlayQuad.ps.h"

namespace trace::render {
namespace {

// Fade duration, inside the 150-180ms band asked for. One constant, used for
// both directions, so appearing and disappearing feel symmetrical.
constexpr int kFadeMs = 165;
// Idle time before the overlay hides itself again.
constexpr int kAutoHideMs = 2000;
// Animation tick. 16ms is one frame at 60Hz; the timer only runs while
// something is actually animating.
constexpr int kAnimTickMs = 16;

constexpr double kPanelWidthLogical = 460.0;
constexpr double kPanelHeightLogical = 76.0;
constexpr double kPanelMarginLogical = 28.0;
constexpr double kIconLogical = 30.0;
constexpr double kHandleLogical = 16.0;

// Atlas pixels -> UV. Built from two CORNERS, not from (x, y, w, h): drawQuad
// reads left/top/right/bottom, and QRectF's four-argument constructor takes a
// width and a height, so passing u1/v1 there silently yields right = u0 + u1.
// That mistake samples a plausible-looking but wrong region of the atlas, which
// is exactly what it did on the first run.
QRectF uvOf(const QRectF& px, QSize atlas) {
    if (atlas.isEmpty()) return QRectF(QPointF(0, 0), QPointF(1, 1));
    return QRectF(QPointF(px.left() / atlas.width(), px.top() / atlas.height()),
                  QPointF(px.right() / atlas.width(), px.bottom() / atlas.height()));
}

void paintPlay(QPainter& p, const QRectF& r) {
    QPainterPath path;
    path.moveTo(r.left() + r.width() * 0.24, r.top() + r.height() * 0.16);
    path.lineTo(r.left() + r.width() * 0.82, r.center().y());
    path.lineTo(r.left() + r.width() * 0.24, r.bottom() - r.height() * 0.16);
    path.closeSubpath();
    p.fillPath(path, QColor(245, 245, 245));
}

void paintPause(QPainter& p, const QRectF& r) {
    const double w = r.width() * 0.18;
    p.fillRect(QRectF(r.left() + r.width() * 0.26, r.top() + r.height() * 0.16,
                      w, r.height() * 0.68), QColor(245, 245, 245));
    p.fillRect(QRectF(r.right() - r.width() * 0.26 - w, r.top() + r.height() * 0.16,
                      w, r.height() * 0.68), QColor(245, 245, 245));
}

void paintChevrons(QPainter& p, const QRectF& r, bool forward) {
    const double y0 = r.top() + r.height() * 0.22;
    const double y1 = r.bottom() - r.height() * 0.22;
    const double mid = r.center().y();
    for (int i = 0; i < 2; ++i) {
        const double x0 = r.left() + r.width() * (0.16 + i * 0.34);
        const double x1 = x0 + r.width() * 0.30;
        QPainterPath path;
        if (forward) {
            path.moveTo(x0, y0);
            path.lineTo(x1, mid);
            path.lineTo(x0, y1);
        } else {
            path.moveTo(x1, y0);
            path.lineTo(x0, mid);
            path.lineTo(x1, y1);
        }
        path.closeSubpath();
        p.fillPath(path, QColor(235, 235, 235));
    }
}

} // namespace

OverlayCompositor::OverlayCompositor() {
    animTimer_.setInterval(kAnimTickMs);
    animTimer_.setSingleShot(false);
    QObject::connect(&animTimer_, &QTimer::timeout, [this]() { tickAnimation(); });

    autoHideTimer_.setInterval(kAutoHideMs);
    autoHideTimer_.setSingleShot(true);
    QObject::connect(&autoHideTimer_, &QTimer::timeout, [this]() {
        // Never hide under the pointer or mid-drag: an overlay that vanishes
        // while being used is worse than one that never appears.
        if (hover_ != Region::None || draggingTimeline_) { autoHideTimer_.start(); return; }
        targetOpacity_ = 0.0;
        startAnimation();
    });
}

OverlayCompositor::~OverlayCompositor() = default;

bool OverlayCompositor::initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                   QString& error) {
    device_ = device;
    context_ = context;
    if (!device_ || !context_) { error = QStringLiteral("overlay: no device"); return false; }
    return createPipeline(error);
}

bool OverlayCompositor::createPipeline(QString& error) {
    if (FAILED(device_->CreateVertexShader(g_TraceOverlayQuadVS, sizeof(g_TraceOverlayQuadVS),
                                           nullptr, &vs_))) {
        error = QStringLiteral("overlay: CreateVertexShader failed");
        return false;
    }
    if (FAILED(device_->CreatePixelShader(g_TraceOverlayQuadPS, sizeof(g_TraceOverlayQuadPS),
                                          nullptr, &ps_))) {
        error = QStringLiteral("overlay: CreatePixelShader failed");
        return false;
    }

    D3D11_BUFFER_DESC bd = {};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DYNAMIC;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bd, nullptr, &constants_))) {
        error = QStringLiteral("overlay: CreateBuffer failed");
        return false;
    }

    // Premultiplied source, so ONE rather than SRC_ALPHA. Pairing this with a
    // non-premultiplied atlas is the classic way to get dark fringes around
    // every glyph.
    D3D11_BLEND_DESC bl = {};
    bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&bl, &blend_))) {
        error = QStringLiteral("overlay: CreateBlendState failed");
        return false;
    }

    D3D11_SAMPLER_DESC sd = {};
    sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&sd, &sampler_))) {
        error = QStringLiteral("overlay: CreateSamplerState failed");
        return false;
    }

    D3D11_RASTERIZER_DESC rd = {};
    rd.FillMode = D3D11_FILL_SOLID;
    rd.CullMode = D3D11_CULL_NONE;
    rd.DepthClipEnable = TRUE;
    if (FAILED(device_->CreateRasterizerState(&rd, &raster_))) {
        error = QStringLiteral("overlay: CreateRasterizerState failed");
        return false;
    }
    return true;
}

void OverlayCompositor::setSurfaceSize(QSize devicePixels) {
    if (devicePixels == surfaceSize_) return;
    surfaceSize_ = devicePixels;
    // Content-dependent, so the atlas is stale. This is one of only three
    // things that can make it stale; a fade or a play/pause is not one of them.
    atlasDirty_ = true;
    layout();
}

void OverlayCompositor::setDevicePixelRatio(double dpr) {
    const double clamped = std::clamp(dpr, 0.5, 8.0);
    if (std::abs(clamped - dpr_) < 1e-6) return;
    dpr_ = clamped;
    atlasDirty_ = true;
    textCached_.clear();  // font pixel size is dpr-derived
    layout();
}

void OverlayCompositor::layout() {
    if (surfaceSize_.isEmpty()) return;

    const double s = dpr_;
    const double panelW = std::min<double>(kPanelWidthLogical * s, surfaceSize_.width() * 0.9);
    const double panelH = kPanelHeightLogical * s;
    const double margin = kPanelMarginLogical * s;
    const double left = (surfaceSize_.width() - panelW) / 2.0;
    const double top = surfaceSize_.height() - panelH - margin;
    dPanel_ = QRectF(left, top, panelW, panelH);

    const double icon = kIconLogical * s;
    const double iconY = top + (panelH * 0.30) - icon / 2.0;
    const double cx = dPanel_.center().x();
    const double gap = icon * 1.9;
    dPlay_ = QRectF(cx - icon / 2.0, iconY, icon, icon);
    dRewind_ = QRectF(cx - gap - icon / 2.0, iconY, icon, icon);
    dFfwd_ = QRectF(cx + gap - icon / 2.0, iconY, icon, icon);

    const double trackInset = 24.0 * s;
    const double trackY = top + panelH * 0.72;
    dTrack_ = QRectF(left + trackInset, trackY - 2.0 * s,
                     panelW - trackInset * 2.0, 4.0 * s);
}

void OverlayCompositor::rebuildAtlas() {
    const double s = dpr_;
    const double icon = kIconLogical * s;
    const double handle = kHandleLogical * s;
    const double panelW = dPanel_.width();
    const double panelH = dPanel_.height();

    // Layout the atlas: panel on top, then a row of icons.
    const int atlasW = static_cast<int>(std::ceil(std::max(panelW, icon * 4 + handle + 40)));
    const int atlasH = static_cast<int>(std::ceil(panelH + icon + 16));
    if (atlasW <= 0 || atlasH <= 0) return;

    QImage image(atlasW, atlasH, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);

    aPanel_ = QRectF(0, 0, panelW, panelH);
    QPainterPath panel;
    panel.addRoundedRect(aPanel_.adjusted(0.5, 0.5, -0.5, -0.5), 10.0 * s, 10.0 * s);
    // Translucent, which is the whole reason this is composited in the render
    // pass rather than stacked as a native window.
    p.fillPath(panel, QColor(18, 18, 20, 165));
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.drawPath(panel);

    const double row = panelH + 8;
    double x = 4;
    aPlay_ = QRectF(x, row, icon, icon);   paintPlay(p, aPlay_);      x += icon + 4;
    aPause_ = QRectF(x, row, icon, icon);  paintPause(p, aPause_);    x += icon + 4;
    aRewind_ = QRectF(x, row, icon, icon); paintChevrons(p, aRewind_, false); x += icon + 4;
    aFfwd_ = QRectF(x, row, icon, icon);   paintChevrons(p, aFfwd_, true);    x += icon + 4;

    aHandle_ = QRectF(x, row, handle, handle);
    p.setBrush(QColor(255, 255, 255));
    p.setPen(Qt::NoPen);
    p.drawEllipse(aHandle_.adjusted(1, 1, -1, -1));
    x += handle + 4;

    // A plain white patch, tinted at draw time. Lets the timeline track and its
    // filled portion be two draws of one texel rather than two more images.
    aSolid_ = QRectF(x, row, 8, 8);
    p.fillRect(aSolid_, QColor(255, 255, 255));

    p.end();

    if (uploadImage(image, atlas_, atlasSrv_, atlasSize_)) atlasDirty_ = false;
}

void OverlayCompositor::rebuildText() {
    const QString text = hooks_.rateText ? hooks_.rateText() : QString();
    if (text == textCached_ && textSrv_) return;
    textCached_ = text;
    if (text.isEmpty()) { textSrv_.Reset(); textTex_.Reset(); return; }

    QFont font;
    font.setPixelSize(static_cast<int>(15 * dpr_));
    font.setBold(true);
    const QFontMetrics fm(font);
    const QSize sz = fm.size(Qt::TextSingleLine, text) + QSize(8, 6);

    QImage image(sz, QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setFont(font);
    p.setPen(QColor(235, 235, 235));
    p.drawText(QRect(QPoint(0, 0), sz), Qt::AlignCenter, text);
    p.end();

    uploadImage(image, textTex_, textSrv_, textSize_);
}

bool OverlayCompositor::uploadImage(const QImage& image, ComPtr<ID3D11Texture2D>& texture,
                                    ComPtr<ID3D11ShaderResourceView>& srv, QSize& sizeOut) {
    if (image.isNull() || !device_) return false;

    // Recreated only when the size changes, which for the atlas means a resize
    // or DPI change. Content-only updates reuse the texture via Map.
    if (!texture || sizeOut != image.size()) {
        texture.Reset();
        srv.Reset();
        D3D11_TEXTURE2D_DESC td = {};
        td.Width = static_cast<UINT>(image.width());
        td.Height = static_cast<UINT>(image.height());
        td.MipLevels = 1;
        td.ArraySize = 1;
        td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        td.SampleDesc.Count = 1;
        td.Usage = D3D11_USAGE_DYNAMIC;
        td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &texture))) return false;
        if (FAILED(device_->CreateShaderResourceView(texture.Get(), nullptr, &srv))) {
            texture.Reset();
            return false;
        }
        sizeOut = image.size();
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(texture.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return false;
    const int rowBytes = image.width() * 4;
    auto* dst = static_cast<uint8_t*>(mapped.pData);
    for (int y = 0; y < image.height(); ++y) {
        std::memcpy(dst + static_cast<size_t>(y) * mapped.RowPitch,
                    image.constScanLine(y), static_cast<size_t>(rowBytes));
    }
    context_->Unmap(texture.Get(), 0);
    return true;
}

void OverlayCompositor::drawQuad(const QRectF& dst, const QRectF& srcUv, float alpha,
                                 const ComPtr<ID3D11ShaderResourceView>& srv,
                                 QSize surfacePixels, float brighten) {
    if (!srv || alpha <= 0.001f) return;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(constants_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) return;
    auto* c = static_cast<Constants*>(mapped.pData);
    c->dstRect[0] = static_cast<float>(dst.x());
    c->dstRect[1] = static_cast<float>(dst.y());
    c->dstRect[2] = static_cast<float>(dst.width());
    c->dstRect[3] = static_cast<float>(dst.height());
    c->srcRect[0] = static_cast<float>(srcUv.left());
    c->srcRect[1] = static_cast<float>(srcUv.top());
    c->srcRect[2] = static_cast<float>(srcUv.right());
    c->srcRect[3] = static_cast<float>(srcUv.bottom());
    // Premultiplied: scaling all four channels by the same term is the correct
    // fade, and `brighten` is how hover/press are expressed without a second
    // set of art.
    c->tint[0] = brighten * alpha;
    c->tint[1] = brighten * alpha;
    c->tint[2] = brighten * alpha;
    c->tint[3] = alpha;
    c->viewport[0] = static_cast<float>(surfacePixels.width());
    c->viewport[1] = static_cast<float>(surfacePixels.height());
    c->viewport[2] = 0.0f;
    c->viewport[3] = 0.0f;
    context_->Unmap(constants_.Get(), 0);

    ID3D11Buffer* cbs[] = {constants_.Get()};
    context_->VSSetConstantBuffers(0, 1, cbs);
    context_->PSSetConstantBuffers(0, 1, cbs);
    ID3D11ShaderResourceView* srvs[] = {srv.Get()};
    context_->PSSetShaderResources(0, 1, srvs);
    context_->Draw(4, 0);
}

void OverlayCompositor::draw(QSize surfacePixels) {
    if (!enabled_ || !context_ || surfacePixels.isEmpty()) return;
    if (opacity_ <= 0.001) return;

    setSurfaceSize(surfacePixels);
    if (atlasDirty_) rebuildAtlas();
    rebuildText();
    if (!atlasSrv_) return;

    // Full-surface viewport: the overlay is positioned against the window, not
    // against the video rect, and the video's letterbox viewport is still set
    // from the frame draw.
    D3D11_VIEWPORT vp = {};
    vp.Width = static_cast<float>(surfacePixels.width());
    vp.Height = static_cast<float>(surfacePixels.height());
    vp.MaxDepth = 1.0f;
    context_->RSSetViewports(1, &vp);
    context_->RSSetState(raster_.Get());
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->IASetInputLayout(nullptr);
    context_->VSSetShader(vs_.Get(), nullptr, 0);
    context_->PSSetShader(ps_.Get(), nullptr, 0);
    ID3D11SamplerState* samplers[] = {sampler_.Get()};
    context_->PSSetSamplers(0, 1, samplers);
    const float blendFactor[4] = {0, 0, 0, 0};
    context_->OMSetBlendState(blend_.Get(), blendFactor, 0xffffffff);

    const float a = static_cast<float>(opacity_);
    const auto hoverBoost = [&](Region r) {
        if (pressed_ == r) return 0.72f;
        if (hover_ == r) return 1.35f;
        return 1.0f;
    };

    drawQuad(dPanel_, uvOf(aPanel_, atlasSize_), a, atlasSrv_, surfacePixels);

    const bool playing = hooks_.isPlaying && hooks_.isPlaying();
    // A different source rect, not a different texture: this is the state
    // change the caching design exists to make free.
    drawQuad(dPlay_, uvOf(playing ? aPause_ : aPlay_, atlasSize_), a, atlasSrv_,
             surfacePixels, hoverBoost(Region::PlayPause));
    drawQuad(dRewind_, uvOf(aRewind_, atlasSize_), a, atlasSrv_, surfacePixels,
             hoverBoost(Region::Rewind));
    drawQuad(dFfwd_, uvOf(aFfwd_, atlasSize_), a, atlasSrv_, surfacePixels,
             hoverBoost(Region::FastForward));

    // Track, filled portion, then handle. All three are the same white patch
    // tinted differently, so none of them can invalidate the atlas.
    const QRectF solidUv = uvOf(aSolid_, atlasSize_);
    drawQuad(dTrack_, solidUv, a * 0.35f, atlasSrv_, surfacePixels);
    const double frac = std::clamp(hooks_.positionFraction ? hooks_.positionFraction() : 0.0,
                                   0.0, 1.0);
    QRectF filled = dTrack_;
    filled.setWidth(dTrack_.width() * frac);
    drawQuad(filled, solidUv, a * 0.85f, atlasSrv_, surfacePixels);

    // Moving the handle is a destination-rect change only.
    const double handleD = kHandleLogical * dpr_;
    const QRectF handleRect(dTrack_.left() + dTrack_.width() * frac - handleD / 2.0,
                            dTrack_.center().y() - handleD / 2.0, handleD, handleD);
    drawQuad(handleRect, uvOf(aHandle_, atlasSize_), a, atlasSrv_, surfacePixels,
             hover_ == Region::Timeline || draggingTimeline_ ? 1.0f : 0.85f);

    if (textSrv_ && !textSize_.isEmpty()) {
        const QRectF textRect(dPanel_.right() - textSize_.width() - 14 * dpr_,
                              dPanel_.top() + 10 * dpr_,
                              textSize_.width(), textSize_.height());
        drawQuad(textRect, QRectF(QPointF(0, 0), QPointF(1, 1)), a, textSrv_, surfacePixels);
    }

    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
}

OverlayCompositor::Region OverlayCompositor::regionAt(int x, int y) const {
    const QPointF p(x, y);
    // Generous vertical band around the track, because a 4px line is not a
    // pointer target.
    QRectF trackHit = dTrack_.adjusted(-8 * dpr_, -12 * dpr_, 8 * dpr_, 12 * dpr_);
    if (dPlay_.contains(p)) return Region::PlayPause;
    if (dRewind_.contains(p)) return Region::Rewind;
    if (dFfwd_.contains(p)) return Region::FastForward;
    if (trackHit.contains(p)) return Region::Timeline;
    return Region::None;
}

void OverlayCompositor::reveal() {
    targetOpacity_ = 1.0;
    startAnimation();
    autoHideTimer_.start();
}

bool OverlayCompositor::onMouseMove(int x, int y) {
    if (!enabled_) return false;
    reveal();

    if (draggingTimeline_) {
        const double frac = std::clamp((x - dTrack_.left()) / std::max(1.0, dTrack_.width()),
                                       0.0, 1.0);
        if (hooks_.seekToFraction) hooks_.seekToFraction(frac);
        if (hooks_.requestRepaint) hooks_.requestRepaint();
        return true;
    }

    const Region was = hover_;
    hover_ = visible() ? regionAt(x, y) : Region::None;
    if (was != hover_ && hooks_.requestRepaint) hooks_.requestRepaint();
    return hover_ != Region::None;
}

bool OverlayCompositor::onMouseDown(int x, int y) {
    if (!enabled_ || !visible()) return false;
    const Region r = regionAt(x, y);
    if (r == Region::None) return false;

    pressed_ = r;
    if (r == Region::Timeline) {
        draggingTimeline_ = true;
        // Press first, then position: this is the slider's own order, and
        // going through it is what gives the overlay the real scrub path.
        if (hooks_.setScrubbing) hooks_.setScrubbing(true);
        const double frac = std::clamp((x - dTrack_.left()) / std::max(1.0, dTrack_.width()),
                                       0.0, 1.0);
        if (hooks_.seekToFraction) hooks_.seekToFraction(frac);
    }
    if (hooks_.requestRepaint) hooks_.requestRepaint();
    return true;
}

bool OverlayCompositor::onMouseUp(int x, int y) {
    if (!enabled_) return false;
    const Region r = regionAt(x, y);
    const Region was = pressed_;
    pressed_ = Region::None;

    if (draggingTimeline_) {
        draggingTimeline_ = false;
        if (hooks_.setScrubbing) hooks_.setScrubbing(false);
        if (hooks_.requestRepaint) hooks_.requestRepaint();
        return true;
    }

    // Click only when press and release landed on the same control, which is
    // what every other button in the app does.
    if (was != Region::None && was == r) {
        switch (r) {
            case Region::PlayPause:   if (hooks_.playPause) hooks_.playPause(); break;
            case Region::Rewind:      if (hooks_.stepBack) hooks_.stepBack(); break;
            case Region::FastForward: if (hooks_.stepForward) hooks_.stepForward(); break;
            default: break;
        }
        if (hooks_.requestRepaint) hooks_.requestRepaint();
        return true;
    }
    if (hooks_.requestRepaint) hooks_.requestRepaint();
    return was != Region::None;
}

void OverlayCompositor::onMouseLeave() {
    if (hover_ == Region::None && pressed_ == Region::None) return;
    hover_ = Region::None;
    if (!draggingTimeline_) pressed_ = Region::None;
    autoHideTimer_.start();
    if (hooks_.requestRepaint) hooks_.requestRepaint();
}

void OverlayCompositor::startAnimation() {
    if (std::abs(targetOpacity_ - opacity_) < 0.001) return;
    if (!fadeClock_.isValid()) fadeClock_.start();
    if (!animTimer_.isActive()) {
        fadeClock_.restart();
        animTimer_.start();
    }
}

void OverlayCompositor::tickAnimation() {
    const double step = static_cast<double>(fadeClock_.restart()) / kFadeMs;
    if (opacity_ < targetOpacity_) opacity_ = std::min(targetOpacity_, opacity_ + step);
    else opacity_ = std::max(targetOpacity_, opacity_ - step);

    if (std::abs(targetOpacity_ - opacity_) < 0.001) {
        opacity_ = targetOpacity_;
        animTimer_.stop();
        if (opacity_ <= 0.0) hover_ = Region::None;
    }
    if (hooks_.requestRepaint) hooks_.requestRepaint();
}

} // namespace trace::render
