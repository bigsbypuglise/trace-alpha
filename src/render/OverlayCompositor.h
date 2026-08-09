#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTimer>

#include <array>
#include <functional>

#include <d3d11.h>
#include <wrl/client.h>

#include "render/OverlayHooks.h"

namespace trace::render {

// DISPOSABLE SPIKE -- proves a renderer-composited transport overlay on the
// child-HWND surface. Not the final interface: the geometry and glyphs here are
// placeholders, and the whole file is meant to be deleted or promoted once the
// approach is accepted.
//
// It exists because the Qt-widget route is closed. Ordinary Qt children over the
// native surface are neither visible nor hit-testable (plan section 18.4), and
// every native-window variant loses translucency. Compositing in the render pass
// is the only route that keeps a translucent panel over GPU-presented video.
//
// The cost discipline is the point of the design, not a detail:
//
//   - The atlas is rasterised with QPainter ONCE and re-rasterised only when its
//     content changes -- surface size, DPI, or theme. Not per frame, not per
//     state change, not per fade step.
//   - Play/pause is a different SOURCE RECT into the same atlas, so the state
//     change is a constant-buffer write.
//   - The timeline handle moves by changing its DESTINATION RECT. Same.
//   - Opacity is a scalar in the tint. Fading never touches a texture.
//   - The rate text lives in its own small texture so that changing "5x" does
//     not invalidate the panel and icons.
//   - Every D3D resource is created once and reused; nothing is allocated in
//     the draw path.
//
// Coordinate space is DEVICE PIXELS of the surface window, everywhere: atlas
// rasterisation, destination rects, hit regions and incoming WM_MOUSE*
// coordinates are all in it. There is deliberately no second space to convert
// between.
class OverlayCompositor {
public:
    // Which control a point is over. Also the identity used for hover/press, so
    // "what is under the pointer" and "what was pressed" cannot disagree.
    enum class Region { None, PlayPause, Rewind, FastForward, Timeline };

    OverlayCompositor();
    ~OverlayCompositor();

    bool initialize(ID3D11Device* device, ID3D11DeviceContext* context, QString& error);
    void setHooks(const OverlayHooks& hooks) { hooks_ = hooks; }
    bool enabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }

    // Surface size changed (resize, DPI, fullscreen). Invalidates the atlas and
    // recomputes every rect; nothing else in the class caches geometry.
    void setSurfaceSize(QSize devicePixels);
    // Scale for art and hit targets. A change invalidates the atlas, which is
    // the third and last thing that can.
    void setDevicePixelRatio(double dpr);

    // Drawn after the video, with the full-surface viewport. Rebuilds the atlas
    // only if it is stale.
    void draw(QSize surfacePixels);

    // --- input, in surface device pixels -------------------------------------
    // Return true when the overlay consumed the event, so the caller knows
    // whether to fall through to default window handling.
    bool onMouseMove(int x, int y);
    bool onMouseDown(int x, int y);
    bool onMouseUp(int x, int y);
    void onMouseLeave();

    // Pointer motion anywhere over the video reveals the overlay and restarts
    // the auto-hide timer, which is the behaviour a floating transport needs.
    void reveal();

    Region hoverRegion() const { return hover_; }
    bool visible() const { return opacity_ > 0.001; }

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
    void rebuildAtlas();
    void rebuildText();
    void layout();
    bool uploadImage(const QImage& image, ComPtr<ID3D11Texture2D>& texture,
                     ComPtr<ID3D11ShaderResourceView>& srv, QSize& sizeOut);
    void drawQuad(const QRectF& dst, const QRectF& srcUv, float alpha,
                  const ComPtr<ID3D11ShaderResourceView>& srv, QSize surfacePixels,
                  float brighten = 1.0f);
    Region regionAt(int x, int y) const;
    void startAnimation();
    void tickAnimation();

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
    ComPtr<ID3D11Texture2D> textTex_;
    ComPtr<ID3D11ShaderResourceView> textSrv_;
    QSize textSize_;
    QString textCached_;

    // Atlas sub-rects, in atlas pixels. Converted to UV at draw time.
    QRectF aPanel_, aPlay_, aPause_, aRewind_, aFfwd_, aHandle_, aSolid_;
    // Destination rects, in surface device pixels.
    QRectF dPanel_, dPlay_, dRewind_, dFfwd_, dTrack_;

    QSize surfaceSize_;
    double dpr_ = 1.0;
    bool atlasDirty_ = true;
    bool enabled_ = false;

    Region hover_ = Region::None;
    Region pressed_ = Region::None;
    bool draggingTimeline_ = false;

    // Fade. targetOpacity_ is where it is going; opacity_ is where it is.
    double opacity_ = 0.0;
    double targetOpacity_ = 0.0;
    QElapsedTimer fadeClock_;
    QTimer animTimer_;
    QTimer autoHideTimer_;

    OverlayHooks hooks_;
};

} // namespace trace::render
