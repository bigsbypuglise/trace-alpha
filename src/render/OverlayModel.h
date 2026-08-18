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
    enum class Source { Atlas, Text, Message, Empty, Readout };

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
    // IN LEFT-TO-RIGHT SCREEN ORDER as of UI redesign roadmap step 5, because
    // controlRects() walks this and that order becomes the screen-reader
    // reading order. Timeline sits where it is drawn -- between the readouts,
    // after Mute -- rather than last, which is where it used to be when the
    // panel had four buttons on one row above it.
    enum class Region {
        None,
        GoToStart,
        Rewind,
        PlayPause,
        FastForward,
        GoToEnd,
        Mute,
        Loop,
        Timeline,
        Fullscreen,
        Share
    };

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
    // Device pixels of the surface's TOP EDGE that the transient top chrome
    // covers (UI redesign roadmap step 7), or 0 when there is no such chrome.
    //
    // TWO READERS, AND BOTH ARE CHROME RATHER THAN PICTURE. The design's
    // empty-state markup puts its stage BELOW the 38px strip and centres the
    // mark in that, which is also what the window did while the menu bar was in
    // the layout; a strip that floats over the picture instead would otherwise
    // leave the mark half a strip high in the only state the strip is
    // permanently shown in.
    //
    // The transient MESSAGE reads it for a blunter reason: it is top-left with a
    // 12px margin, so under a 38px floating strip it was drawn entirely behind
    // the chrome. That was invisible while the menu bar was in the layout -- the
    // video surface started below it -- and step 7 made the chrome float over
    // the picture without moving the toast out from under it.
    //
    // The transport is bottom-anchored and unaffected, and neither reader is a
    // picture, so this still cannot move one.
    void setTopInset(double devicePixels);

    // The single reveal state both panels are driven from -- true from the
    // moment a fade-in is asked for until a fade-out has finished. It is what
    // `OverlayHooks::setChromeRevealed` reports; this exposes it for readers
    // that need to ASK rather than be told, so there is still exactly one
    // answer to "is the chrome on screen" and no second flag to fall out of
    // step with it. See syncChrome() for why it is not `visible()`.
    bool chromeRevealed() const { return chromeRevealed_; }

    void setDevicePixelRatio(double dpr);
    double devicePixelRatio() const { return dpr_; }

    // Rebuilds whatever is stale and returns the quads to draw, in order.
    // EMPTY when there is nothing to show, which is the backend's cue to do no
    // work at all -- including no state changes on the device.
    const std::vector<OverlayQuad>& buildFrame(QSize surfacePixels);

    const QImage& atlasImage() const { return atlas_; }
    const QImage& textImage() const { return text_; }
    // The transient message pill (see OverlayHooks::messageText). A third image
    // rather than a second use of text_ because the rate chip and a message can
    // be on screen at once, and because this one is emitted even at opacity 0 --
    // a confirmation or an error must not vanish with the panel's fade.
    const QImage& messageImage() const { return message_; }
    // The polished empty state (UI redesign roadmap step 3): the prism mark and
    // its hint line, rasterised together. A FOURTH image rather than drawing
    // code in each backend, because it used to be exactly that -- the literal
    // and its QPainter call existed independently in CpuImageRenderer and
    // D3D11VideoRenderer, which is the shape that produced a sub-pixel fit
    // divergence the last time this project had one expression written twice.
    //
    // It is also not part of the transport, and buildFrame() emits it OUTSIDE
    // both gates to say so: outside the opacity gate, like the message, because
    // it must not fade; and outside `enabled_`, because it is what the window
    // IS with no media open and must therefore survive TRACE_TRANSPORT_BAR=1.
    const QImage& emptyImage() const { return empty_; }
    // The two time readouts either side of the track (UI redesign roadmap step
    // 5), rasterised into ONE image with two known sub-rects.
    //
    // A FIFTH image rather than a second use of text_, because the rate chip
    // and the readouts change on completely different schedules -- the chip on
    // a shuttle press, the readouts on every frame of playback -- and sharing
    // one image would re-rasterise the chip 24 times a second. One image for
    // both readouts rather than two, because they always change together: the
    // position moves and the duration is re-measured against the same font in
    // the same rebuild, so two images would be two revisions and two uploads
    // for one event.
    const QImage& readoutImage() const { return readout_; }
    long long atlasRevision() const { return atlasRevision_; }
    long long textRevision() const { return textRevision_; }
    long long messageRevision() const { return messageRevision_; }
    long long emptyRevision() const { return emptyRevision_; }
    long long readoutRevision() const { return readoutRevision_; }

    // Whether the renderer has a frame to show. The empty state is the negative
    // of this and of nothing else.
    //
    // ASKED OF THE RENDERER RATHER THAN OF THE APPLICATION, and the difference
    // is the "does not flash between two opens" requirement. MainWindow's
    // currentMedia_ would answer this too, but openPath() releases the outgoing
    // media before the incoming frame arrives, and a first open that FAILS
    // leaves currentMedia_ set with nothing on screen. "There is no picture" is
    // the condition that is true at startup, true after Close Media, false
    // throughout a file change -- the viewer holds the outgoing frame until the
    // incoming one lands -- and true when an open failed. Each backend passes
    // the same member its own draw branch reads, so the two cannot disagree.
    void setMediaPresent(bool present) { mediaPresent_ = present; }
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
    void rebuildMessage(QSize surfacePixels);
    void rebuildEmpty(QSize surfacePixels);
    void rebuildReadout();
    Region regionAt(int x, int y) const;
    // The track's HIT rect, which is much taller than its drawn one. One
    // definition, shared by regionAt() and controlRects().
    QRectF trackHitRect() const;
    void startAnimation();
    void tickAnimation();
    // Pushes the top chrome's shown/hidden state at the host, on transitions
    // only. Called from every place the fade state can change, so there is one
    // reveal state driving both panels rather than a second timer for the top.
    void syncChrome();
    double fractionAt(int x) const;

    QImage atlas_;
    QImage text_;
    QImage message_;
    QImage empty_;
    QImage readout_;
    long long atlasRevision_ = 0;
    long long textRevision_ = 0;
    long long messageRevision_ = 0;
    long long emptyRevision_ = 0;
    long long readoutRevision_ = 0;
    long long layoutRevision_ = 0;
    QString textCached_;
    QString messageCached_;
    QString positionCached_;
    QString durationCached_;
    // Cache key for empty_. The mark is a FIXED logical size, so the only thing
    // that can change its device size is the DPI -- which is why a resize is
    // free here even though the image is rebuilt at the size it is drawn at.
    // The hint is elided against the surface width, so it joins the key.
    QString emptyTextCached_;
    double emptyMarkPx_ = 0.0;

    // Atlas sub-rects, in atlas pixels.
    //
    // aStrip_ IS ONE NARROW COLUMN, NOT THE WHOLE STRIP, and that is deliberate.
    // The strip is now the full width of the window, so a full-width cell would
    // be a 5120x56 image on this box's panel, rebuilt on every resize. Its
    // gradient varies only VERTICALLY, so a column stretched horizontally
    // reproduces it exactly -- there is no scaling along y, and along x every
    // sampled column is identical, so bilinear returns the same value. Sampled
    // from its inner columns for the same reason aSolid_ is: a source rect
    // flush with the cell edge pulls the neighbouring cell in and fringes the
    // ends.
    QRectF aStrip_, aStripSample_;
    QRectF aPlay_, aPause_;
    QRectF aGoToStart_, aRewind_, aFfwd_, aGoToEnd_;
    QRectF aVolume_, aVolumeMuted_, aLoop_, aLoopOn_, aFullscreen_, aExitFullscreen_, aShare_;
    QRectF aThumb_, aThumbScrub_;
    QRectF aSolid_, aSolidSample_, aAccent_, aAccentSample_;
    // The design's own alphas, baked in rather than applied at draw time --
    // see rebuildAtlas(). A fractional alpha applied by two different
    // compositors is a measured cross-backend difference.
    QRectF aTrackBg_, aTrackBgSample_, aSeparator_, aSeparatorSample_;
    // Four plate cells: two states x two control sizes, each a rounded rect
    // at the size of the control it sits behind. No sample rect, because
    // nothing is stretched -- these are 1:1 like every glyph.
    QRectF aPlateUtil_, aPlateUtilPressed_, aPlatePlay_, aPlatePlayPressed_;
    // Destination rects, in surface device pixels.
    QRectF dStrip_, dPlay_, dGoToStart_, dRewind_, dFfwd_, dGoToEnd_, dMute_, dLoop_;
    QRectF dFullscreen_, dShare_, dSeparator_, dTrack_;
    // The two readout cells. Empty when the strip is too narrow to carry them
    // -- see layout()'s elision -- which is also how buildFrame knows not to
    // draw them.
    QRectF dPosition_, dDuration_;

    // Snapped control sizes in device pixels, computed in layout() and reused
    // by rebuildAtlas() so an atlas cell is exactly the size of the rect it is
    // drawn into. Recomputing the constants there instead is how the two would
    // end up a pixel apart and resample.
    //
    // TWO icon sizes as of spec phase 6, because the approved package specifies
    // a 44x44 play/pause and 34x34 utility targets. They are separate members
    // for the same reason they are snapped at all: the atlas cell for each
    // control has to be exactly the rect it lands in.
    // THE SIGNED-OFF 44/34 PAIR IS SUPERSEDED, KNOWINGLY. Spec phase 6 settled
    // a 460x84 panel with 44x44 play and 34x34 utility controls and the owner's
    // sign-off recorded "no tuning is wanted" -- so those were settled numbers
    // rather than defaults. UI redesign roadmap step 5 replaces them with the
    // design package's edge-to-edge strip, and the roadmap says in terms that
    // this SUPERSEDES that sign-off rather than drifting from it. kFadeMs and
    // kAutoHideMs are explicitly NOT superseded and are unchanged.
    double playPx_ = 40.0;
    double utilPx_ = 36.0;
    double thumbPx_ = 13.0;
    double thumbScrubPx_ = 16.0;
    double stripPx_ = 56.0;
    // The width reserved for each readout cell, in device pixels. Measured from
    // the DURATION's text rather than the position's, because the position can
    // never be wider than the duration in the same mode -- so the track's
    // geometry stays put as the digits change instead of resizing on every
    // frame of playback, which would bump layoutRevision_ 24 times a second and
    // re-sync the accessibility proxies with it.
    double readoutPx_ = 0.0;

    QSize surfaceSize_;
    double topInset_ = 0.0;
    double dpr_ = 1.0;
    bool atlasDirty_ = true;
    bool enabled_ = false;
    bool mediaPresent_ = false;

    Region hover_ = Region::None;
    Region pressed_ = Region::None;
    // A pan drag on the picture (spec phase 15). Kept beside the timeline drag
    // because it is the same kind of state -- a gesture in progress that
    // outlives any one event -- and because the two are mutually exclusive by
    // construction: a press either lands on a control or it does not.
    bool panning_ = false;
    int panLastX_ = 0;
    int panLastY_ = 0;
    bool draggingTimeline_ = false;
    // Mirrors what the host was last told, so reveal() on every pointer move
    // does not call across the hook once per move to say the same thing. The
    // chrome mirror is the same idea and exists for the same reason: showing a
    // native window is not free and reveal() runs per pointer sample.
    bool cursorHidden_ = false;
    bool chromeRevealed_ = false;

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
