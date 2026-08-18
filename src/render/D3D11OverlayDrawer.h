#pragma once

#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>

#include <d3d11.h>
#include <wrl/client.h>

#include "render/OverlayModel.h"

namespace trace::render {

// Puts an OverlayModel's quads on the D3D11 back buffer, and does nothing else.
//
// Everything that decides WHAT to draw -- layout, artwork, hover, fade,
// hit-testing, the command hooks -- lives in OverlayModel, because the CPU
// backend needs all of it too and `TRACE_RENDERER=cpu` is the escape hatch
// users are told to reach for. What is left here is the part that genuinely
// cannot be shared: a pipeline, two dynamic textures, and one constant-buffer
// write per quad.
//
// The two textures are re-uploaded only when the model says its images changed,
// which it reports as a REVISION rather than as a flag or an image comparison.
// A revision that does not move is the promise that backs "the overlay does not
// upload anything per frame" -- the requirement this whole design exists to
// meet, and the one a per-frame QPainter rasterise would silently break.
//
// Coordinate space is DEVICE PIXELS of the surface window, everywhere: model
// geometry, destination rects, hit regions and incoming WM_MOUSE* coordinates
// are all in it. There is deliberately no second space to convert between.
class D3D11OverlayDrawer {
public:
    D3D11OverlayDrawer() = default;
    ~D3D11OverlayDrawer() = default;

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context, QString& error);

    // Drawn after the video, with the full-surface viewport. Returns without
    // touching a single piece of device state when the model has nothing to
    // show, so a hidden overlay costs one predictable branch per present.
    void draw(OverlayModel& model, QSize surfacePixels);

private:
    template <typename T> using ComPtr = Microsoft::WRL::ComPtr<T>;

    struct Constants {
        float dstRect[4];
        float srcRect[4];
        float tint[4];
        float viewport[4];
    };
    static_assert(sizeof(Constants) == 64, "constant buffer must stay 16-byte aligned");

    bool createPipeline(QString& error);
    bool syncTexture(const QImage& image, long long revision, long long& cachedRevision,
                     ComPtr<ID3D11Texture2D>& texture, ComPtr<ID3D11ShaderResourceView>& srv,
                     QSize& sizeOut);
    void drawQuad(const QRectF& dst, const QRectF& srcUv, float alpha, float brighten,
                  const ComPtr<ID3D11ShaderResourceView>& srv, QSize surfacePixels);

    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11Buffer> constants_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11SamplerState> sampler_;
    ComPtr<ID3D11RasterizerState> raster_;

    ComPtr<ID3D11Texture2D> atlas_;
    ComPtr<ID3D11ShaderResourceView> atlasSrv_;
    QSize atlasSize_;
    long long atlasRevisionUploaded_ = -1;

    ComPtr<ID3D11Texture2D> textTex_;
    ComPtr<ID3D11ShaderResourceView> textSrv_;
    QSize textSize_;
    long long textRevisionUploaded_ = -1;

    ComPtr<ID3D11Texture2D> msgTex_;
    ComPtr<ID3D11ShaderResourceView> msgSrv_;
    QSize msgSize_;
    long long msgRevisionUploaded_ = -1;

    // The empty state (roadmap step 3). A fourth texture on the same terms as
    // the other three: uploaded only when its revision moves, and released by
    // syncTexture when the model drops the image because media opened.
    ComPtr<ID3D11Texture2D> emptyTex_;
    ComPtr<ID3D11ShaderResourceView> emptySrv_;
    QSize emptySize_;
    long long emptyRevisionUploaded_ = -1;
};

} // namespace trace::render
