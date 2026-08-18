#include "render/D3D11OverlayDrawer.h"

#include <cstring>

#include "OverlayQuad.vs.h"
#include "OverlayQuad.ps.h"

namespace trace::render {
namespace {

// Image pixels -> UV. Built from two CORNERS, not from (x, y, w, h): drawQuad
// reads left/top/right/bottom, and QRectF's four-argument constructor takes a
// width and a height, so passing u1/v1 there silently yields right = u0 + u1.
// That mistake samples a plausible-looking but wrong region of the atlas, which
// is exactly what it did on the first run of the spike.
QRectF uvOf(const QRectF& px, QSize image) {
    if (image.isEmpty()) return QRectF(QPointF(0, 0), QPointF(1, 1));
    return QRectF(QPointF(px.left() / image.width(), px.top() / image.height()),
                  QPointF(px.right() / image.width(), px.bottom() / image.height()));
}

} // namespace

bool D3D11OverlayDrawer::initialize(ID3D11Device* device, ID3D11DeviceContext* context,
                                    QString& error) {
    device_ = device;
    context_ = context;
    if (!device_ || !context_) { error = QStringLiteral("overlay: no device"); return false; }
    return createPipeline(error);
}

bool D3D11OverlayDrawer::createPipeline(QString& error) {
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
    // every glyph. It is also, deliberately, the same arithmetic Qt's
    // SourceOver performs on ARGB32_Premultiplied, which is what lets the CPU
    // backend draw the same quads and land on the same pixels.
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

bool D3D11OverlayDrawer::syncTexture(const QImage& image, long long revision,
                                     long long& cachedRevision,
                                     ComPtr<ID3D11Texture2D>& texture,
                                     ComPtr<ID3D11ShaderResourceView>& srv, QSize& sizeOut) {
    if (image.isNull() || !device_) {
        if (revision != cachedRevision) { texture.Reset(); srv.Reset(); sizeOut = QSize(); }
        cachedRevision = revision;
        return false;
    }
    // The whole point of the revision: an unchanged overlay does not reach the
    // Map/memcpy below at all, however many frames are presented over it.
    if (revision == cachedRevision && srv) return true;

    // Recreated only when the size changes, which for the atlas means a resize
    // or a DPI change. Content-only updates reuse the texture via Map.
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
    cachedRevision = revision;
    return true;
}

void D3D11OverlayDrawer::drawQuad(const QRectF& dst, const QRectF& srcUv, float alpha,
                                  float brighten, const ComPtr<ID3D11ShaderResourceView>& srv,
                                  QSize surfacePixels) {
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

void D3D11OverlayDrawer::draw(OverlayModel& model, QSize surfacePixels) {
    if (!context_ || surfacePixels.isEmpty()) return;

    const auto& quads = model.buildFrame(surfacePixels);
    // Nothing to show: return before any device state is touched. A hidden
    // overlay must cost one branch, not a viewport and a blend state.
    if (quads.empty()) return;

    syncTexture(model.atlasImage(), model.atlasRevision(), atlasRevisionUploaded_,
                atlas_, atlasSrv_, atlasSize_);
    syncTexture(model.textImage(), model.textRevision(), textRevisionUploaded_,
                textTex_, textSrv_, textSize_);
    syncTexture(model.messageImage(), model.messageRevision(), msgRevisionUploaded_,
                msgTex_, msgSrv_, msgSize_);
    syncTexture(model.emptyImage(), model.emptyRevision(), emptyRevisionUploaded_,
                emptyTex_, emptySrv_, emptySize_);
    syncTexture(model.readoutImage(), model.readoutRevision(), readoutRevisionUploaded_,
                readoutTex_, readoutSrv_, readoutSize_);
    // No blanket atlas guard: a message quad can be the only thing to show, and
    // it can arrive while the panel is faded -- the one path on which the atlas
    // may never have been rasterised. Each quad checks its own source in the
    // loop, which is the same rule the CPU drawer follows.

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

    // THE SHAPE OF THIS LOOP IS LOAD-BEARING AND HAS ALREADY COST ONE SESSION.
    // Adding the message quad by selecting the SRV through a pointer-to-ComPtr
    // plus a switch made this drawer draw NO QUADS AT ALL -- the whole transport
    // panel vanished on the default renderer while the CPU backend was fine --
    // and every line of it read as equivalent to what it replaced. It was found
    // by bisecting against a control build, not by reading. So the empty state
    // is one more case on the existing ternaries, and a rewrite of this loop
    // that "reads the same" is not something to accept on inspection.
    for (const auto& q : quads) {
        const bool isText = q.source == OverlayQuad::Source::Text;
        const bool isMsg = q.source == OverlayQuad::Source::Message;
        const bool isEmpty = q.source == OverlayQuad::Source::Empty;
        const bool isReadout = q.source == OverlayQuad::Source::Readout;
        if (isText && !textSrv_) continue;
        if (isMsg && !msgSrv_) continue;
        if (isEmpty && !emptySrv_) continue;
        if (isReadout && !readoutSrv_) continue;
        if (!isText && !isMsg && !isEmpty && !isReadout && !atlasSrv_) continue;
        drawQuad(q.dst,
                 uvOf(q.src, isReadout ? readoutSize_
                                       : (isEmpty ? emptySize_
                                                  : (isMsg ? msgSize_
                                                           : (isText ? textSize_
                                                                     : atlasSize_)))),
                 q.alpha, q.brighten,
                 isReadout ? readoutSrv_
                           : (isEmpty ? emptySrv_
                                      : (isMsg ? msgSrv_
                                               : (isText ? textSrv_ : atlasSrv_))),
                 surfacePixels);
    }

    context_->OMSetBlendState(nullptr, blendFactor, 0xffffffff);
}

} // namespace trace::render
