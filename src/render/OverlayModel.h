#pragma once

#include <QElapsedTimer>
#include <QImage>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QTimer>

#include <vector>

#include "render/OverlayHooks.h"

namespace trace::render {

// One textured rectangle of the overlay, in the only two coordinate spaces
// that exist here: `dst` in DEVICE PIXELS of the surface, `src` in PIXELS of
// whichever source image it names. A backend converts src to whatever its own
// sampler wants -- UV for D3D11, a QRect for QPainter -- and nothing else.
//
// `alpha` scales all four channels; `brighten` scales RGB only, which is how
// hover and press are expressed without a second set of art. Both backends
// implement the same arithmetic: premultiplied source composited with
// `dst = src + dst * (1 - srcAlpha)`, which is what D3D11's ONE/INV_SRC_ALPHA
// and Qt's SourceOver on ARGB32_Premultiplied each already do. That is why the
// two paths can be compared pixel for pixel rather than merely "looking right".
struct OverlayQuad {
    enum class Source { Atlas, Text };

    QRectF dst;
    QRectF src;
    float alpha = 1.0f;
    float brighten = 1.0f;
    Source source = Source::Atlas;
};

// Everything about the floating transport that is not a draw call.
//
// This exists because the overlay has to work on BOTH backends and used to
// exist only on one. `d3d11` is the default renderer as of 2026-08-10, but
// `TRACE_RENDERER=cpu` is the documented escape hatch -- the first thing to try
// if the picture looks wrong -- and a transport that lives inside the D3D11
// pipeline would vanish the moment a user took that advice. So layout, art,
// state, hit-testing, fade and the command hooks live here, backend-agnostic,
// and each backend implements only "put these quads on the screen".
//
// The cost discipline from the spike is preserved exactly, and it is the
// content of the design rather than a detail:
//
//   - The atlas is rasterised with QPainter ONCE and re-rasterised only when
//     its content changes -- surface size, DPI, or theme. Not per frame, not
//     per state change, not per fade step.
//   - Play/pause is a different SOURCE RECT into the same atlas.
//   - The timeline handle moves by changing its DESTINATION RECT.
//   - Opacity and hover are scalars on a quad. Fading never touches an image.
//   - The rate text lives in its own small image, so changing "5x" does not
//     invalidate the panel and icons.
//
// `atlasRevision()` / `textRevision()` are how a backend that caches an upload
// knows when to redo it WITHOUT comparing images. A revision that does not move
// is a promise that the pixels did not, and it is the promise the "no per-frame
// upload" requirement is actually made of.
class OverlayModel {
public:
    // Which control a point is over. Also the identity used for hover and
    // press, so "what is under the pointer" and "what was pressed" cannot
    // disagree.
    enum class Region { None, PlayPause, Rewind, FastForward, Share, Timeline };

    OverlayModel();
    ~OverlayModel();

    OverlayModel(const OverlayModel&) = delete;
    OverlayModel& operator=(const OverlayModel&) = delete;

    void setHooks(const OverlayHooks& hooks) { hooks_ = hooks; }
    bool enabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }

    // Whether the environment asks for the overlay at all. Read once, and read
    // here rather than in a backend, because the answer must be the same on
    // both -- an overlay that appears only on the default renderer would be a
    // difference between the shipping path and its own control.
    static bool enabledByEnvironment();

    void setSurfaceSize(QSize devicePixels);
    void setDevicePixelRatio(double dpr);
    double devicePixelRatio() const { return dpr_; }

    // Rebuilds whatever is stale and returns the quads to draw, in order.
    // EMPTY when there is nothing to show, which is the backend's cue to do no
    // work at all -- including no state changes on the device.
    const std::vector<OverlayQuad>& buildFrame(QSize surfacePixels);

    const QImage& atlasImage() const { return atlas_; }
    const QImage& textImage() const { return text_; }
    long long atlasRevision() const { return atlasRevision_; }
    long long textRevision() const { return textRevision_; }
    // Bumped every time layout() moves a control rect. Same promise the atlas
    // and text revisions make -- a revision that does not move means the rects
    // did not -- and it exists for the same reason: so a consumer can tell
    // whether to redo work WITHOUT comparing geometry.
    //
    // Its consumer is the accessibility proxy tree, which has to reposition
    // itself when the panel does. It cannot be driven from the viewer's resize
    // alone: layout() runs inside buildFrame(), i.e. during the PAINT, so a
    // proxy synced on resize reads the rects from before the layout that resize
    // caused. Measured -- the proxies were exposed to UI Automation with an
    // EMPTY bounding rectangle, so a screen reader could name every control and
    // locate none of them.
    long long layoutRevision() const { return layoutRevision_; }

    // --- input, in surface device pixels -------------------------------------
    // Return true when the overlay consumed the event, so the caller knows
    // whether to fall through to its default handling.
    bool onMouseMove(int x, int y);
    bool onMouseDown(int x, int y);
    bool onMouseUp(int x, int y);
    // Double-click the video toggles fullscreen. Routed through the model
    // because the model is the renderer-neutral home for input over the video
    // surface, and it runs even when the overlay itself is switched off -- the
    // gesture belongs to the window, not to the transport.
    bool onMouseDoubleClick(int x, int y);
    void onMouseLeave();

    // Pointer motion anywhere over the video reveals the overlay and restarts
    // the auto-hide timer, which is the behaviour a floating transport needs.
    // Also the host's entry point for the two reveal sources that are not mouse
    // events: a click on the video, and relevant keyboard input.
    void reveal();

    // The interactive controls and where they are, in SURFACE DEVICE PIXELS.
    //
    // Exists so spec phase 14's accessibility proxy tree can be positioned from
    // THE SAME RECTS the compositor draws and hit-tests, rather than from a
    // second layout that agrees today. Plan section 19.7 asks for "one
    // zero-painting Qt widget per control, positioned on the same rects the
    // compositor lays out", and the only way for that to stay true through a
    // panel resize, a DPI change or a future layout change is for there to be
    // one set of rects. A screen reader announcing a button that moved two
    // phases ago is not a bug anyone would notice from the picture.
    //
    // Region::None is never returned; Region::Timeline is, because the track is
    // a control even though it is not a button.
    struct ControlRect {
        Region region = Region::None;
        QRectF dst;
    };
    std::vector<ControlRect> controlRects() const;

    Region hoverRegion() const { return hover_; }
    bool visible() const { return opacity_ > 0.001; }
    // True while the overlay is mid-fade. The host uses it to decide whether a
    // repaint request is animation or damage.
    bool animating() const { return animTimer_.isActive(); }

private:
    void layout();
    void rebuildAtlas();
    void rebuildText();
    Region regionAt(int x, int y) const;
    // The track's HIT rect, which is much taller than its drawn one. One
    // definition, shared by regionAt() and controlRects().
    QRectF trackHitRect() const;
    void startAnimation();
    void tickAnimation();
    double fractionAt(int x) const;

    QImage atlas_;
    QImage text_;
    long long atlasRevision_ = 0;
    long long textRevision_ = 0;
    long long layoutRevision_ = 0;
    QString textCached_;

    // Atlas sub-rects, in atlas pixels.
    QRectF aPanel_, aPlay_, aPause_, aRewind_, aFfwd_, aShare_, aHandle_, aSolid_, aSolidSample_;
    // Destination rects, in surface device pixels.
    QRectF dPanel_, dPlay_, dRewind_, dFfwd_, dShare_, dTrack_;

    // Snapped control sizes in device pixels, computed in layout() and reused
    // by rebuildAtlas() so an atlas cell is exactly the size of the rect it is
    // drawn into. Recomputing the constants there instead is how the two would
    // end up a pixel apart and resample.
    //
    // TWO icon sizes as of spec phase 6, because the approved package specifies
    // a 44x44 play/pause and 34x34 utility targets. They are separate members
    // for the same reason they are snapped at all: the atlas cell for each
    // control has to be exactly the rect it lands in.
    double playPx_ = 44.0;
    double utilPx_ = 34.0;
    double handlePx_ = 16.0;

    QSize surfaceSize_;
    double dpr_ = 1.0;
    bool atlasDirty_ = true;
    bool enabled_ = false;

    Region hover_ = Region::None;
    Region pressed_ = Region::None;
    bool draggingTimeline_ = false;
    // Mirrors what the host was last told, so reveal() on every pointer move
    // does not call across the hook once per move to say the same thing.
    bool cursorHidden_ = false;

    // Fade. targetOpacity_ is where it is going; opacity_ is where it is.
    double opacity_ = 0.0;
    double targetOpacity_ = 0.0;
    QElapsedTimer fadeClock_;
    QTimer animTimer_;
    QTimer autoHideTimer_;

    // Rebuilt in place each frame rather than reallocated: the draw path
    // allocating once per present is exactly the per-frame cost this design
    // exists to not have.
    std::vector<OverlayQuad> quads_;

    OverlayHooks hooks_;
};

} // namespace trace::render
