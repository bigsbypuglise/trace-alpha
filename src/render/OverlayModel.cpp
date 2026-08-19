#include "render/OverlayModel.h"

#include <QBrush>
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QLinearGradient>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QtGlobal>

#include <QElapsedTimer>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <initializer_list>

namespace trace::render {
namespace {

// TRACE_REVEAL_LOG=1 (owner items 3+7, 2026-08-18): one stderr line per call
// to reveal(), per auto-hide decision, per chrome show/hide transition and per
// mouse-move sample, each with a monotonic timestamp and a source tag. It
// exists because the owner sees the chrome cycling with the cursor STATIONARY,
// which contradicts the recorded behaviour (a parked pointer holds the chrome
// up indefinitely) -- so something is calling reveal() with no input, and the
// project's standing rule is to attribute it by measurement rather than by
// reading. A stationary cursor should produce ZERO mousemove lines; synthetic
// WM_MOUSEMOVEs (which Windows posts when the window under the pointer
// changes, e.g. a native strip showing or hiding beneath it) show up here as
// repeated moves at an unchanged coordinate.
bool revealLogEnabled() {
    static const bool on = [] {
        const QByteArray v = qgetenv("TRACE_REVEAL_LOG");
        return !v.isEmpty() && v != "0";
    }();
    return on;
}

qint64 revealLogMs() {
    static QElapsedTimer clock;
    if (!clock.isValid()) clock.start();
    return clock.elapsed();
}

// Rounds to a whole DEVICE pixel. Shared by layout() and rebuildAtlas() so a
// cell and the rect it is drawn into land on the same integer -- one of them
// rounding differently is exactly the sub-pixel offset that made the play glyph
// differ across the two backends on 8.1% of its pixels before the layout was
// snapped.
double snapTo(double v) { return std::floor(v + 0.5); }


// Fade duration, inside the 150-180ms band asked for. One constant, used for
// both directions, so appearing and disappearing feel symmetrical.
constexpr int kFadeMs = 165;
// Idle time before the overlay hides itself again. The spec's provisional
// figure.
constexpr int kAutoHideMs = 2000;
// Animation tick. 16ms is one frame at 60Hz; the timer only runs while
// something is actually animating, which is what keeps a hidden overlay free.
constexpr int kAnimTickMs = 16;

// THE EDGE-TO-EDGE TRANSPORT STRIP (UI redesign roadmap step 5), and every
// number here is the design package's own -- its HANDOFF.md, "Geometry in the
// mockups": *"Strip 56 px, edge-to-edge - play/pause 40 px, other controls
// 36 px, radius 6 - timeline track 4 px (6 px on hover), thumb 13 px (16 px
// scrubbing) - accent only on the played track + thumb ring."*
//
// THIS SUPERSEDES SPEC PHASE 6's SIGNED-OFF 460x84 PANEL, and the roadmap says
// so in terms rather than leaving it to be discovered as drift: that sign-off
// recorded "no tuning is wanted", which made 460/84/44/34 settled numbers, and
// the owner has replaced them deliberately. kFadeMs and kAutoHideMs above are
// NOT superseded -- the auto-hide's feel was accepted separately and is
// untouched here.
//
// Note the mockup MARKUP renders the same strip at 52/38/34 rather than
// 56/40/36; HANDOFF.md's geometry line is the spec and the mockup is a
// rendering of it at a demo window size, which is why the spec's numbers are
// the ones taken. The arrangement -- buttons, position, track, duration,
// fullscreen, separator, share -- is the mockup's exactly.
constexpr double kStripHeightLogical = 56.0;
constexpr double kStripPadLogical = 14.0;
// Between the button cluster and the readout, and between the right group and
// the window edge. The mockup's flex `gap: 12px`.
constexpr double kGroupGapLogical = 12.0;
// Inside the left cluster. The mockup's inner `gap: 2px` -- the buttons read as
// one group because their hit cells touch, not because they are drawn joined.
constexpr double kButtonGapLogical = 2.0;
// Between a readout and the end of the track.
constexpr double kReadoutGapLogical = 10.0;
constexpr double kPlayLogical = 40.0;
constexpr double kUtilLogical = 36.0;
// The hover/press plate behind a control. Radius 6 from the same handoff line.
constexpr double kControlRadiusLogical = 6.0;
constexpr double kTrackLogical = 4.0;
constexpr double kTrackHoverLogical = 6.0;
constexpr double kThumbLogical = 13.0;
// The handoff's 16px scrubbing thumb is REMOVED (owner item 17, 2026-08-18):
// the size change mid-drag is what made the thumb pop and shimmer (item 16 --
// the same resample class step 5 measured at 12,511 differing pixels on a
// stretched cell), and the owner chose a fixed-size thumb over the design's
// grow-on-scrub. One cell, one size, always a 1:1 blit.
// The ring around the thumb, drawn in the accent at low alpha. The mockup's
// `0 0 0 3px rgba(90,200,232,0.33)`, which is a 3px spread outside the dot.
constexpr double kThumbRingLogical = 3.0;
constexpr double kReadoutTextLogical = 12.0;
// The mockup's `width:1px; height:18px; background:rgba(255,255,255,0.14)`
// between Fullscreen and Share.
constexpr double kSeparatorHeightLogical = 18.0;
// Below this the track is not worth drawing and something has to give; see the
// elision in layout().
constexpr double kTrackMinLogical = 48.0;

// THE ACCENT, AND IT IS THE ONLY PLACE COLOUR APPEARS IN THE TRANSPORT.
// `#5AC8E8` from HANDOFF.md, applied to the played portion of the track and to
// the thumb's ring and to nothing else -- which is the handoff's own rule
// ("accent only on the played track + thumb ring") and is what keeps the strip
// reading as chrome rather than competing with the picture.
constexpr int kAccentR = 0x5A;
constexpr int kAccentG = 0xC8;
constexpr int kAccentB = 0xE8;

// The mockup's utility glyphs are `rgba(255,255,255,0.72)` and its play glyph
// is `#FFF`. It is baked into the ATLAS CELL as a grey rather than applied as
// an alpha, because `brighten` scales RGB only: white ink at full alpha is
// already clamped, so a hover on it changes nothing but the antialiased edge,
// while grey ink lifts to 247 and is visibly brighter. Dimming by alpha instead
// would break the premultiplied invariant the moment brighten exceeded 1.
constexpr int kUtilInk = 183;  // 0.72 * 255

// THE EMPTY STATE, AND EVERY NUMBER HERE IS THE DESIGN PACKAGE'S OWN.
// assets/source/260817-trace-ui-v2/Trace-App-Mockups.html, section screen-3:
// an <svg width="104" height="104"> above a 14px line, in a flex column with
// `gap: 22px`, centred on both axes.
//
// THEY ARE FIXED LOGICAL SIZES, NOT A FRACTION OF THE WINDOW, and that is the
// design rather than a simplification -- the mark stays 104 logical px in a
// 460px portrait window and in a 5120px fullscreen one. It is worth knowing
// that this was checked against the delivered mockup PNG rather than assumed:
// its client area is 832x483 and its mark canvas measures 103, which is the
// 104 above at that render's 0.9917 scale.
constexpr double kEmptyMarkLogical = 104.0;
constexpr double kEmptyGapLogical = 22.0;
constexpr double kEmptyTextLogical = 14.0;
// rgba(255, 255, 255, 0.42) from the same markup. Over the black stage that is
// the (107, 107, 107) the mockup's own pixels read, and expressing it as white
// at an alpha rather than as a flat grey is what keeps it correct if the stage
// is ever not black.
constexpr int kEmptyTextAlpha = 107;

// U+203A, and the mockup writes it with a non-breaking space either side.
// The two backends each carried their own copy of a plainer version of this
// string; that duplication is what step 3 removes.
QString emptyHintText() {
    return QStringLiteral("Drop media or File › Open");
}

// The approved artwork, drawn into the atlas cell.
//
// This is a rasterisation, not a per-frame cost: it happens inside
// rebuildAtlas(), which runs only when the surface size, the DPI or the theme
// changes. And it does not disturb the property the atlas exists for -- the cell
// is still exactly the size of the destination rect layout() snapped, so both
// backends still blit it 1:1 and still agree pixel for pixel. Only what is
// inside the cell changed.
//
// WHICH GLYPH IS CHOSEN BY WHAT THE CONTROL DOES, not by what it will do. The
// two side controls run the single-frame step actions today, so they carry the
// frame-step artwork; the approved package's transport_scan_* pair is the
// artwork for the shuttle and goes on at spec phases 4 and 5, in the same commit
// as the behaviour. Before this the overlay drew continuous-scan chevrons over
// stepping behaviour and the mismatch was recorded rather than fixed -- that was
// the right call while the art was placeholder, and the wrong state to keep once
// it is real.
//
// THE RENDITION SIZES ARE A PARAMETER because the empty-state mark is not on
// the 24/48 ladder -- it is drawn at 104 logical px, so it ships 104 and 208,
// which is the same @1x/@2x pair the controls have at their own drawn size.
// Every candidate is existence-checked, which subsumes the special case the
// -72 line used to be rather than adding a second one beside it.
void paintIcon(QPainter& p, const QRectF& r, const QString& baseName,
               std::initializer_list<int> sizes = {24, 48, 72}) {
    // Cached by base name: an atlas rebuild is rare, but a QIcon assembled from
    // resources on each one is avoidable work for no benefit.
    static QHash<QString, QIcon> cache;
    auto it = cache.find(baseName);
    if (it == cache.end()) {
        QIcon icon;
        for (const int px : sizes) {
            const QString path = QStringLiteral(":/ui/%1-%2.png").arg(baseName).arg(px);
            if (QFile::exists(path)) icon.addFile(path, QSize(px, px));
        }
        it = cache.insert(baseName, icon);
    }

    const QSize cell(std::max(1, static_cast<int>(std::lround(r.width()))),
                     std::max(1, static_cast<int>(std::lround(r.height()))));
    const QPixmap pm = it.value().pixmap(cell);
    if (pm.isNull()) return;
    p.drawPixmap(r.topLeft(), pm);
}

// The same, tinted. The delivered glyphs are white, and the design draws the
// utility controls at 72% while play/pause stays full white.
//
// Done through a scratch image and CompositionMode_SourceIn rather than by
// setting an opacity on the painter, for the reason kUtilInk records: the tint
// has to end up in the RGB of an OPAQUE pixel, so that `brighten` -- which
// scales RGB and leaves alpha alone -- has somewhere to go on hover. Tinting by
// alpha would leave brighten pushing RGB past alpha, which is not a
// representable premultiplied colour and clamps differently on the two
// backends. The scratch image is per-cell and rebuildAtlas() is rare.
void paintIconTinted(QPainter& p, const QRectF& r, const QString& baseName,
                     std::initializer_list<int> sizes, const QColor& ink) {
    const int w = std::max(1, static_cast<int>(std::lround(r.width())));
    const int h = std::max(1, static_cast<int>(std::lround(r.height())));
    QImage cell(w, h, QImage::Format_ARGB32_Premultiplied);
    if (cell.isNull()) return;
    cell.fill(Qt::transparent);
    {
        QPainter cp(&cell);
        cp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        paintIcon(cp, QRectF(0, 0, w, h), baseName, sizes);
        cp.setCompositionMode(QPainter::CompositionMode_SourceIn);
        cp.fillRect(QRect(0, 0, w, h), ink);
    }
    p.drawImage(r.topLeft(), cell);
}

} // namespace

// THE FLOATING TRANSPORT IS THE DEFAULT AS OF SPEC PHASE 6, and this one
// function decides it for the whole application. MainWindow asks it whether to
// put the docked bar in the layout and ViewerWidget asks it whether to draw the
// overlay, so the two cannot disagree and there is no combination of knobs that
// leaves the window with no transport at all.
//
// `TRACE_TRANSPORT_BAR=1` is the control and the escape hatch: it restores the
// docked bar exactly as it shipped through phase 5. It is what the eight
// measurement scripts that locate the timeline by scanning for its groove need
// in order to keep running unchanged, and it is the negative control for every
// figure this phase produces.
//
// Three names are accepted, which is two more than a fresh design would have.
// `TRACE_OVERLAY` is the phase-2 name and `TRACE_OVERLAY_COMPOSITED` the spike's;
// both are kept because `overlay.ps1` and `overlay_cost.ps1` set them, and
// silently breaking the instrument that has to prove this change is free would
// be the wrong kind of tidy. Either set to 0 now selects bar mode, so turning
// the overlay off means asking for the other transport rather than for none.
bool OverlayModel::enabledByEnvironment() {
    static const bool on = [] {
        const QByteArray bar = qgetenv("TRACE_TRANSPORT_BAR");
        if (!bar.isEmpty() && bar != "0") return false;
        const QByteArray a = qgetenv("TRACE_OVERLAY");
        if (!a.isEmpty()) return a != "0";
        const QByteArray b = qgetenv("TRACE_OVERLAY_COMPOSITED");
        if (!b.isEmpty()) return b != "0";
        return true;
    }();
    return on;
}

OverlayModel::OverlayModel() {
    animTimer_.setInterval(kAnimTickMs);
    animTimer_.setSingleShot(false);
    QObject::connect(&animTimer_, &QTimer::timeout, [this]() { tickAnimation(); });

    autoHideTimer_.setInterval(kAutoHideMs);
    autoHideTimer_.setSingleShot(true);
    QObject::connect(&autoHideTimer_, &QTimer::timeout, [this]() {
        // Never hide under the pointer or mid-drag: an overlay that vanishes
        // while being used is worse than one that never appears. The rest of
        // the spec's hold list -- a popup menu, a tooltip, a child control
        // holding focus -- is application state the overlay cannot see, so it
        // is asked for rather than guessed at.
        if (hover_ != Region::None || draggingTimeline_
            || (hooks_.holdVisible && hooks_.holdVisible())) {
            if (revealLogEnabled()) {
                fprintf(stderr, "[reveal] %8lld ms  auto-hide HELD  hover %d drag %d hostHold %d\n",
                        static_cast<long long>(revealLogMs()), static_cast<int>(hover_),
                        draggingTimeline_ ? 1 : 0,
                        (hooks_.holdVisible && hooks_.holdVisible()) ? 1 : 0);
                fflush(stderr);
            }
            autoHideTimer_.start();
            return;
        }
        if (revealLogEnabled()) {
            fprintf(stderr, "[reveal] %8lld ms  auto-hide FIRED -> fading out\n",
                    static_cast<long long>(revealLogMs()));
            fflush(stderr);
        }
        targetOpacity_ = 0.0;
        startAnimation();
        // The cursor goes with the panel and on the same idle timer, which is
        // what makes "the interface got out of the way" one event rather than
        // two that happen to be close together. Whether it is actually hidden
        // is the host's call -- the spec asks for it in fullscreen only.
        if (!cursorHidden_) {
            cursorHidden_ = true;
            if (hooks_.setCursorHidden) hooks_.setCursorHidden(true);
        }
    });

    // Reserve once. The draw path must not allocate, and the quad count is
    // fixed by the design rather than by the media.
    quads_.reserve(12);
}

OverlayModel::~OverlayModel() = default;

void OverlayModel::setSurfaceSize(QSize devicePixels) {
    if (devicePixels == surfaceSize_) return;
    surfaceSize_ = devicePixels;
    // Content-dependent, so the atlas is stale. This is one of only three
    // things that can make it stale; a fade or a play/pause is not one of them.
    atlasDirty_ = true;
    layout();
}

void OverlayModel::setTopInset(double devicePixels) {
    const double v = std::max(0.0, devicePixels);
    if (std::abs(v - topInset_) < 0.5) return;
    topInset_ = v;
    // Nothing is rebuilt: the inset moves a destination rect and no image
    // depends on it. The next paint picks it up.
    if (hooks_.requestRepaint) hooks_.requestRepaint();
}

void OverlayModel::setDevicePixelRatio(double dpr) {
    const double clamped = std::clamp(dpr, 0.5, 8.0);
    if (std::abs(clamped - dpr_) < 1e-6) return;
    dpr_ = clamped;
    atlasDirty_ = true;
    textCached_.clear();  // font pixel size is dpr-derived
    layout();
}

// Every rect here is snapped to whole DEVICE PIXELS, and that is load-bearing
// rather than tidy. The atlas is rasterised at exactly the size each control is
// drawn at, so an integral destination makes the blit a 1:1 copy on both
// backends -- no resampling, no filter, and therefore no way for D3D11's
// bilinear sampler and QPainter's smooth transform to disagree.
//
// Measured before snapping, on the same frame: the panel body already agreed to
// within a channel delta of 2, but the play glyph differed on 8.1% of its
// pixels with a peak of 29, entirely along its antialiased edges -- two
// resamplers reconstructing the same art at a fractional offset. Snapping
// removes the resample rather than trying to match it, which also makes the
// artwork crisper on both paths.
void OverlayModel::layout() {
    if (surfaceSize_.isEmpty()) return;

    const double s = dpr_;
    const auto snap = [](double v) { return std::floor(v + 0.5); };

    stripPx_ = snap(kStripHeightLogical * s);
    playPx_ = snap(kPlayLogical * s);
    utilPx_ = snap(kUtilLogical * s);
    thumbPx_ = snap(kThumbLogical * s);

    const double pad = snap(kStripPadLogical * s);
    const double groupGap = snap(kGroupGapLogical * s);
    const double buttonGap = snap(kButtonGapLogical * s);
    const double readoutGap = snap(kReadoutGapLogical * s);
    const double sepW = std::max(1.0, snap(1.0 * s));

    // EDGE TO EDGE: the full width of the surface, flush with its bottom. There
    // is no margin and no centring any more -- that was the floating panel's
    // shape, and this is the thing that replaces it.
    const double top = snap(surfaceSize_.height() - stripPx_);
    dStrip_ = QRectF(0, top, surfaceSize_.width(), stripPx_);

    // One centre line for everything on the strip. The controls differ in size,
    // so each rect is derived from that line rather than from a shared top edge
    // -- aligning tops would sit the 36px glyphs 2px above the 40px one.
    const double cy = top + stripPx_ / 2.0;
    const auto cell = [&](double x, double size) {
        return QRectF(snap(x), snap(cy - size / 2.0), size, size);
    };

    // The left cluster: << > >> then mute and loop. Go to Start and Go to End
    // are REMOVED from the strip (owner item 15, 2026-08-18, reversing the
    // step 5 decision with the strip on screen) -- their width goes to the
    // timeline, which is what the owner asked the removal for. Home and End
    // stay bound; only the buttons went.
    double x = pad;
    dRewind_ = cell(x, utilPx_);          x += utilPx_ + buttonGap;
    dPlay_ = cell(x, playPx_);            x += playPx_ + buttonGap;
    dFfwd_ = cell(x, utilPx_);            x += utilPx_ + buttonGap;
    dMute_ = cell(x, utilPx_);            x += utilPx_ + buttonGap;
    dLoop_ = cell(x, utilPx_);            x += utilPx_;
    const double leftEnd = x;

    // The right group, laid out from the right edge inwards: share, separator,
    // fullscreen. Same order as the mockup, which puts share outermost.
    double r = surfaceSize_.width() - pad;
    dShare_ = cell(r - utilPx_, utilPx_);
    r -= utilPx_ + groupGap;
    const double sepH = snap(kSeparatorHeightLogical * s);
    dSeparator_ = QRectF(snap(r - sepW), snap(cy - sepH / 2.0), sepW, sepH);
    r -= sepW + groupGap;
    dFullscreen_ = cell(r - utilPx_, utilPx_);
    r -= utilPx_;
    const double rightStart = r;

    // WHAT IS LEFT IN THE MIDDLE, AND WHAT GIVES WHEN THERE IS NOT ENOUGH.
    //
    // Section 4's window minimum is still the 460 logical px the floating panel
    // set, and at that width the full strip does not fit -- so the readouts are
    // dropped before the track is allowed to become useless. Duration goes
    // first because the position is the more useful of the two, and the
    // buttons and the track are never dropped: a transport that loses its play
    // button on a narrow window would be a worse answer than a tight one.
    const double middle = rightStart - leftEnd - 2.0 * groupGap;
    const double trackMin = snap(kTrackMinLogical * s);
    bool showPosition = readoutPx_ > 0.0;
    bool showDuration = readoutPx_ > 0.0;
    const auto middleNeeds = [&]() {
        double need = trackMin;
        if (showPosition) need += readoutPx_ + readoutGap;
        if (showDuration) need += readoutPx_ + readoutGap;
        return need;
    };
    if (middle < middleNeeds()) showDuration = false;
    if (middle < middleNeeds()) showPosition = false;

    double tx = leftEnd + groupGap;
    double tw = middle;
    dPosition_ = QRectF();
    dDuration_ = QRectF();
    const double readoutH = snap(kReadoutTextLogical * s * 1.4);
    if (showPosition) {
        dPosition_ = QRectF(snap(tx), snap(cy - readoutH / 2.0), readoutPx_, readoutH);
        tx += readoutPx_ + readoutGap;
        tw -= readoutPx_ + readoutGap;
    }
    if (showDuration) {
        tw -= readoutPx_ + readoutGap;
        dDuration_ = QRectF(snap(tx + tw + readoutGap), snap(cy - readoutH / 2.0),
                            readoutPx_, readoutH);
    }

    // dTrack_ IS THE 4px BASE RECT AND STAYS THAT WAY ON HOVER. The design
    // thickens the track to 6px while the pointer is over it and grows the
    // thumb while scrubbing, but both of those are computed in buildFrame from
    // this rect rather than re-run through layout() -- a hover that re-laid out
    // would bump layoutRevision_ and re-sync the accessibility proxies on every
    // pointer sample, for a two-pixel change that moves no control.
    const double trackH = std::max(1.0, snap(kTrackLogical * s));
    dTrack_ = QRectF(snap(tx), snap(cy - trackH / 2.0), std::max(trackMin, tw), trackH);

    // Last, so it is bumped only once every rect above is settled. See
    // layoutRevision() for who reads it and what went wrong without it.
    ++layoutRevision_;
}

// The drawn rects, handed out. Nothing is computed here: this returns exactly
// what layout() put in the destination members and buildFrame() pushed quads
// for, which is what makes the accessibility proxies and the picture the same
// geometry rather than two that match.
//
// THE TIMELINE'S TOUCHABLE HEIGHT IS NOT ITS DRAWN HEIGHT. dTrack_ is four
// device pixels tall -- a hairline -- and the hit test deliberately accepts a
// much taller band around it so the track can be grabbed. A proxy the height of
// the drawn line would be a four-pixel target for anyone driving by touch or by
// a magnifier, so the HIT rect is what is reported. That is also the honest
// answer: it is the region that responds.
//
// Through trackHitRect() rather than by repeating the adjustment, so this and
// regionAt() cannot come to disagree about where the track is -- which would
// show up as a control a screen reader can find and a pointer cannot, or the
// reverse.
std::vector<OverlayModel::ControlRect> OverlayModel::controlRects() const {
    std::vector<ControlRect> rects;
    if (dStrip_.isEmpty()) return rects;
    // LEFT TO RIGHT, which is also the screen-reader reading order. Go to
    // Start and Go to End are gone from the strip (owner item 15, 2026-08-18);
    // Home and End remain the keyboard route and the proxy descriptions for
    // the remaining controls still name their keys.
    rects.reserve(8);
    rects.push_back({Region::Rewind, dRewind_});
    rects.push_back({Region::PlayPause, dPlay_});
    rects.push_back({Region::FastForward, dFfwd_});
    rects.push_back({Region::Mute, dMute_});
    rects.push_back({Region::Loop, dLoop_});
    rects.push_back({Region::Timeline, trackHitRect()});
    rects.push_back({Region::Fullscreen, dFullscreen_});
    rects.push_back({Region::Share, dShare_});
    return rects;
}

// Generous vertical band around the track, because a 4px line is not a pointer
// target. ONE definition, asked by the hit test and by the accessibility
// proxies alike.
QRectF OverlayModel::trackHitRect() const {
    // Vertically it fills the strip's own height, which is what the mockup's
    // 18px-tall flex row does and is more forgiving than the old panel's band.
    // Horizontally it is NOT widened: the readouts sit immediately either side,
    // and a track that reached under them would swallow presses meant for
    // nothing and start a scrub from a point outside the track's own mapping.
    const double half = std::max(0.0, (dStrip_.height() - dTrack_.height()) / 2.0);
    return dTrack_.adjusted(0, -half, 0, half);
}

void OverlayModel::rebuildAtlas() {
    const double s = dpr_;
    // The SAME snapped sizes layout() placed, not the constants recomputed --
    // an atlas cell one pixel off from its destination rect would reintroduce
    // the resample the snapping exists to remove.
    const double play = playPx_;
    const double util = utilPx_;
    const double stripH = stripPx_;
    // Each thumb cell carries its accent ring, so the cell is the dot plus the
    // ring on both sides. Drawing the ring as a separate quad would need a
    // second rect tracking the first to a sub-pixel as it travels.
    const double ring = snapTo(kThumbRingLogical * s);
    const double thumbCell = thumbPx_ + 2.0 * ring;
    const double rowH = std::max({play, util, thumbCell});

    // THE STRIP CELL IS THE FULL WIDTH OF THE STRIP, AND THE FIRST CUT WAS A
    // NARROW COLUMN STRETCHED ACROSS IT. Measured, both backends, over the
    // empty state's black stage so no video could contaminate it: the stretched
    // version differed on 12,511 pixels at max channel delta 3, in whole
    // uniform rows and mostly in blue -- geometry identical, value rounding
    // different, because two resamplers reconstructed one gradient.
    //
    // The reasoning for stretching was that the gradient varies only vertically,
    // so a horizontal stretch is information-preserving. That is true of the
    // SOURCE and says nothing about the two samplers, which is exactly the
    // mistake this project already paid for once -- the play glyph differed on
    // 8.1% of its pixels at max delta 29 purely from a fractional offset, and
    // the rule that came out of it is that every cell is rasterised at the size
    // it is drawn so the blit is 1:1 on both backends. The strip is not an
    // exception to that rule; it was just a bigger cell.
    //
    // The cost is an atlas as wide as the window -- ~1.1MB at 5120 -- rebuilt
    // only when the surface size, the DPI or the theme changes, which is the
    // same set of events that already rebuilt it.
    const double kStripCellW = std::max(1.0, dStrip_.width());

    // play x2, then nine utility glyphs, one thumb cell, the flat patches, the
    // four track end-cap circles and the strip column, with 4px between them.
    // Undercounting here silently clips the LAST cell, which is why the count
    // is spelled out.
    const int atlasW = static_cast<int>(std::ceil(std::max(
        kStripCellW,
        play * 4 + util * 12 + thumbCell + 32 + 64 + 4 * 20)));
    const int atlasH = static_cast<int>(std::ceil(stripH + rowH + 16));
    if (atlasW <= 0 || atlasH <= 0) return;

    QImage image(atlasW, atlasH, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return;
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);

    // The strip's own column. `linear-gradient(to top, rgba(16,16,18,0.55),
    // rgba(16,16,18,0.34))` from the mockup, so the BOTTOM is the more opaque
    // end, plus its 1px top border. Translucent, which is the whole reason this
    // is composited into the render pass rather than stacked as a native window
    // -- and it is exactly what the top chrome could NOT have, because that one
    // is a real HWND with no video pixels to blend against.
    aStrip_ = QRectF(0, 0, kStripCellW, stripH);
    QLinearGradient grad(0, aStrip_.top(), 0, aStrip_.bottom());
    grad.setColorAt(0.0, QColor(16, 16, 18, 87));   // 0.34
    grad.setColorAt(1.0, QColor(16, 16, 18, 140));  // 0.55
    p.fillRect(aStrip_, grad);
    p.fillRect(QRectF(aStrip_.left(), aStrip_.top(), aStrip_.width(),
                      std::max(1.0, snapTo(1.0 * s))),
               QColor(255, 255, 255, 18));  // 0.07
    // The whole cell, with no inset: the inset existed to stop a stretched
    // sample pulling in the neighbouring cell, and nothing is stretched now.
    aStripSample_ = aStrip_;

    // The icon row starts at x=0 on a line BELOW the strip cell rather than
    // beside it: the strip cell is now the full width of the window, so laying
    // the glyphs out after it would make the atlas twice the window wide for no
    // reason.
    const double row = stripH + 8;
    double x = 0;
    const QColor utilInk(kUtilInk, kUtilInk, kUtilInk);
    const auto util3 = [&](QRectF& dst, const char* name) {
        dst = QRectF(x, row, util, util);
        // Inset so the 24/48 glyph is drawn at its own proportion inside the
        // 36px control cell rather than filling it -- the mockup draws a 19-20px
        // glyph in a 34px cell, i.e. a little over half.
        const double inset = snapTo(util * 0.22);
        paintIconTinted(p, dst.adjusted(inset, inset, -inset, -inset),
                        QString::fromLatin1(name), {24, 48}, utilInk);
        x += util + 4;
    };

    // Play and pause stay FULL WHITE, which is the mockup's own distinction:
    // every utility glyph is rgba(255,255,255,0.72) and the play glyph is #FFF.
    const auto playCell = [&](QRectF& dst, const char* name) {
        dst = QRectF(x, row, play, play);
        const double inset = snapTo(play * 0.20);
        paintIcon(p, dst.adjusted(inset, inset, -inset, -inset),
                  QString::fromLatin1(name), {24, 48});
        x += play + 4;
    };

    // Artwork follows behaviour, one control at a time -- and it follows it
    // OUT as well as in: go-to-start and go-to-end left the atlas, the .qrc
    // and the asset tree in the same commit as the buttons (owner item 15,
    // 2026-08-18), exactly as the rule brought them in at step 5.
    util3(aRewind_, "rewind");
    playCell(aPlay_, "play");
    playCell(aPause_, "pause");
    util3(aFfwd_, "fast-forward");
    util3(aVolume_, "volume");
    util3(aVolumeMuted_, "volume-muted");
    util3(aLoop_, "loop");
    // The same glyph in the accent, for Loop ON. Its own cell rather than a
    // draw-time tint, because `brighten` scales RGB uniformly and cannot turn
    // neutral ink cyan -- and because every other cell here is 1:1 for exactly
    // the cross-backend reason the strip's background had to become 1:1.
    {
        aLoopOn_ = QRectF(x, row, util, util);
        const double inset = snapTo(util * 0.22);
        paintIconTinted(p, aLoopOn_.adjusted(inset, inset, -inset, -inset),
                        QStringLiteral("loop"), {24, 48},
                        QColor(kAccentR, kAccentG, kAccentB));
        x += util + 4;
    }
    util3(aFullscreen_, "fullscreen");
    util3(aExitFullscreen_, "exit-fullscreen");
    util3(aShare_, "share");

    // ONE thumb cell, a white dot inside its accent ring, always drawn 1:1.
    // There were two -- 13px plus a 16px grow-while-scrubbing variant from the
    // handoff -- and the swap between them mid-drag is what the owner saw as
    // the thumb popping (items 16+17, 2026-08-18): the cell changed at exactly
    // the moment the user is watching the thumb most closely. The owner removed
    // the scale animation; a single fixed cell cannot resample or pop.
    const auto thumbCellAt = [&](QRectF& dst, double dot) {
        dst = QRectF(x, row, dot + 2.0 * ring, dot + 2.0 * ring);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(kAccentR, kAccentG, kAccentB, 84));  // 0.33
        p.drawEllipse(dst);
        p.setBrush(QColor(255, 255, 255));
        p.drawEllipse(QRectF(dst.left() + ring, dst.top() + ring, dot, dot));
        x += dst.width() + 4;
    };
    thumbCellAt(aThumb_, thumbPx_);

    // TRACK END CAPS (owner item 2, 2026-08-18): the timeline track gets round
    // ends, radius = half the track height. Full circles at the track's two
    // heights (base and hover) in the track's two colours, alphas BAKED IN like
    // every other track cell, sliced in half at draw time -- so a cap is a 1:1
    // blit and cannot fringe or resample the way a stretched rounded rect
    // would. Heights mirror buildFrame's own hover-grow arithmetic exactly, or
    // the cell and its destination rect would disagree by a pixel at
    // fractional DPI.
    const double capBaseH = std::max(1.0, dTrack_.height());
    const double capWantH = std::max(1.0, snapTo(kTrackHoverLogical * s));
    const double capHoverH = capBaseH + 2.0 * snapTo((capWantH - capBaseH) / 2.0);
    const auto circleCell = [&](QRectF& dst, double diameter, const QColor& c) {
        dst = QRectF(x, row, diameter, diameter);
        p.setPen(Qt::NoPen);
        p.setBrush(c);
        p.drawEllipse(dst);
        x += diameter + 4;
    };
    circleCell(aTrackCapBg_, capBaseH, QColor(255, 255, 255, 56));
    circleCell(aTrackCapBgHover_, capHoverH, QColor(255, 255, 255, 56));
    circleCell(aTrackCapAccent_, capBaseH, QColor(kAccentR, kAccentG, kAccentB));
    circleCell(aTrackCapAccentHover_, capHoverH, QColor(kAccentR, kAccentG, kAccentB));

    // Two plain patches, white and accent, tinted by alpha at draw time. Lets
    // the track, its played portion and the separator be three draws of two
    // texels rather than three more images.
    //
    // Drawn 8x8 and SAMPLED from the inner 6x6. These are the quads that are
    // genuinely stretched -- a long thin track from a small square -- so both
    // backends filter them, and a source rect flush with the patch edge would
    // pull the neighbouring cell in and fringe the ends of the track.
    // THE DESIGN'S ALPHAS ARE BAKED INTO THE CELLS RATHER THAN APPLIED AT DRAW
    // TIME, AND THAT IS A MEASURED REQUIREMENT RATHER THAN A PREFERENCE.
    //
    // These patches are stretched into long thin rects, and the first cut drew
    // them from one white cell at `a * 0.22f`, `a * 0.14f` and so on. Over the
    // empty state's black stage the two backends then differed on 3,594 pixels
    // at max channel delta 2 -- entirely the track and the separator, in whole
    // uniform rows -- because a fractional alpha is applied by QPainter's
    // integer SourceOver on one path and by a float multiply in the shader on
    // the other, and 0.22 x 255 = 56.1 does not round the same way twice.
    //
    // With the alpha in the cell, the only multiplier left at draw time is the
    // FADE, which is exactly 1.0 whenever the strip is fully revealed -- so a
    // revealed strip is a plain 1:1 blit of identical premultiplied bytes and
    // the two paths have nothing left to disagree about.
    p.setBrush(Qt::NoBrush);
    const auto patch = [&](QRectF& cell, QRectF& sample, const QColor& c) {
        cell = QRectF(x, row, 8, 8);
        p.fillRect(cell, c);
        // Sampled from the inner 6x6: these ARE stretched, so a source rect
        // flush with the cell edge would pull the neighbouring cell in and
        // fringe the ends of the track.
        sample = cell.adjusted(1, 1, -1, -1);
        x += 12;
    };
    patch(aSolid_, aSolidSample_, QColor(255, 255, 255));
    patch(aAccent_, aAccentSample_, QColor(kAccentR, kAccentG, kAccentB));
    // `rgba(255,255,255,0.22)` -- the unplayed track.
    patch(aTrackBg_, aTrackBgSample_, QColor(255, 255, 255, 56));
    // `rgba(255,255,255,0.14)` -- the separator between Fullscreen and Share.
    patch(aSeparator_, aSeparatorSample_, QColor(255, 255, 255, 36));
    // THE HOVER AND PRESSED PLATES ARE ROUNDED RECTS AT THE CONTROL'S OWN SIZE,
    // not a stretched patch, because the handoff's "radius 6" is a property of
    // the plate's CORNERS and a corner cannot be stretched out of an 8x8 square.
    // Four cells rather than two: the plate has to be exactly the size of the
    // control it sits behind, and there are two control sizes.
    //
    // The alpha is baked in for the same reason the track's is -- see above.
    const double radius = snapTo(kControlRadiusLogical * s);
    const auto plateCell = [&](QRectF& dst, double size, int alpha) {
        dst = QRectF(x, row, size, size);
        QPainterPath rr;
        rr.addRoundedRect(dst, radius, radius);
        p.fillPath(rr, QColor(255, 255, 255, alpha));
        x += size + 4;
    };
    plateCell(aPlateUtil_, util, 26);          // 0.10
    plateCell(aPlateUtilPressed_, util, 41);   // 0.16
    plateCell(aPlatePlay_, play, 26);
    plateCell(aPlatePlayPressed_, play, 41);

    p.end();

    atlas_ = image;
    ++atlasRevision_;
    atlasDirty_ = false;
}

void OverlayModel::rebuildText() {
    const QString text = hooks_.rateText ? hooks_.rateText() : QString();
    if (text == textCached_ && !text_.isNull()) return;
    textCached_ = text;
    if (text.isEmpty()) {
        if (!text_.isNull()) { text_ = QImage(); ++textRevision_; }
        return;
    }

    QFont font;
    font.setPixelSize(std::max(1, static_cast<int>(15 * dpr_)));
    font.setBold(true);
    const QFontMetrics fm(font);
    const QSize sz = fm.size(Qt::TextSingleLine, text) + QSize(8, 6);

    QImage image(sz, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return;
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setFont(font);
    p.setPen(QColor(235, 235, 235));
    p.drawText(QRect(QPoint(0, 0), sz), Qt::AlignCenter, text);
    p.end();

    text_ = image;
    ++textRevision_;
}

// The transient message pill -- the surface that replaced the status bar for
// user-facing text (spec step 4 of the UI redesign roadmap). The pill's
// background is baked into the image itself, so showing a message costs one
// quad and no atlas dependency: it has to draw over any frame of video, and it
// has to draw when the panel has never been shown and the atlas is still null.
//
// Cached on the string alone, exactly as rebuildText is: an elide that would
// change with the surface width is not worth re-rasterising every resize for,
// because a message lives for two to five seconds.
void OverlayModel::rebuildMessage(QSize surfacePixels) {
    const QString text = hooks_.messageText ? hooks_.messageText() : QString();
    if (text == messageCached_ && (text.isEmpty() == message_.isNull())) return;
    messageCached_ = text;
    if (text.isEmpty()) {
        if (!message_.isNull()) { message_ = QImage(); ++messageRevision_; }
        return;
    }

    QFont font;
    font.setPixelSize(std::max(1, static_cast<int>(13 * dpr_)));
    const QFontMetrics fm(font);
    const double padX = 10.0 * dpr_;
    const double padY = 6.0 * dpr_;
    const double margin = 12.0 * dpr_;
    const int maxTextW = std::max(static_cast<int>(32 * dpr_),
                                  static_cast<int>(surfacePixels.width() - 2.0 * (margin + padX)));
    const QString shown = fm.elidedText(text, Qt::ElideRight, maxTextW);
    const QSize textSz = fm.size(Qt::TextSingleLine, shown);
    const QSize sz(textSz.width() + static_cast<int>(2 * padX),
                   textSz.height() + static_cast<int>(2 * padY));

    QImage image(sz, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return;
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    // The panel's own colour family, slightly more opaque: this is read over
    // moving picture, not over a dimmed strip.
    QPainterPath pill;
    pill.addRoundedRect(QRectF(0.5, 0.5, sz.width() - 1.0, sz.height() - 1.0),
                        6.0 * dpr_, 6.0 * dpr_);
    p.fillPath(pill, QColor(18, 18, 20, 205));
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.drawPath(pill);
    p.setPen(QColor(235, 235, 235));
    p.setFont(font);
    p.drawText(QRect(QPoint(0, 0), sz), Qt::AlignCenter, shown);
    p.end();

    message_ = image;
    ++messageRevision_;
}

// THE POLISHED EMPTY STATE (UI redesign roadmap step 3).
//
// One image holding the prism mark above its hint line, laid out exactly as the
// design package's own empty-state markup does it: a 104px mark, a 22px gap and
// a 14px line, centred as a column. Building both into one image is what makes
// the whole state one quad and one texture on each backend, and it is why the
// two renderers now share this instead of each carrying a literal and a
// drawText call of its own.
//
// RASTERISED AT THE SIZE IT IS DRAWN, like every control in the atlas, so the
// blit is a 1:1 copy on both backends and neither resampler gets a chance to
// reconstruct the art differently from the other. That is not a theoretical
// concern here: the play glyph differed across the two backends on 8.1% of its
// pixels at max delta 29 until the layout was snapped, purely from a fractional
// offset.
//
// The rebuild is keyed on the DEVICE mark size and the elided hint, not on the
// surface size. The mark is a fixed logical size, so an ordinary resize changes
// neither and costs nothing; only a DPI change, or a window narrow enough to
// re-elide the hint, rasterises again.
void OverlayModel::rebuildEmpty(QSize surfacePixels) {
    if (mediaPresent_) {
        // Dropped rather than kept hidden, so the backends release the texture
        // and a revision bump tells them to. There is no reason to hold a
        // window-sized image alive for the whole of a playback session.
        if (!empty_.isNull()) {
            empty_ = QImage();
            emptyTextCached_.clear();
            emptyMarkPx_ = 0.0;
            ++emptyRevision_;
        }
        return;
    }

    const double s = dpr_;
    const auto snap = [](double v) { return std::floor(v + 0.5); };
    const double markPx = std::max(1.0, snap(kEmptyMarkLogical * s));

    QFont font;
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(kEmptyTextLogical * s))));
    const QFontMetrics fm(font);
    // Elided against the window rather than allowed to run off it. A 460px
    // portrait window at 200% is the case: the hint is wider than the picture
    // and the mark is not.
    const int maxTextW = std::max(static_cast<int>(markPx),
                                  static_cast<int>(surfacePixels.width() - snap(32.0 * s)));
    const QString shown = fm.elidedText(emptyHintText(), Qt::ElideRight, maxTextW);

    if (!empty_.isNull() && std::abs(markPx - emptyMarkPx_) < 0.5 && shown == emptyTextCached_)
        return;

    const double gap = snap(kEmptyGapLogical * s);
    const double lineH = fm.height();
    const double textW = fm.horizontalAdvance(shown);
    const int w = static_cast<int>(std::ceil(std::max(markPx, textW)));
    const int h = static_cast<int>(std::ceil(markPx + gap + lineH));
    if (w <= 0 || h <= 0) return;

    QImage image(w, h, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return;
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    // THE INK IS CENTRED, NOT THE CANVAS (owner item 5, 2026-08-18 -- an owner
    // OVERRIDE of the delivered art, not a correction of a bug). The mark's
    // canvas is square and its art sits ~9.5px right of the canvas centre by
    // design: a right-pointing triangle balanced by eye rather than by its
    // bounding box. Trace reproduced that to within a pixel and the owner
    // prefers the mark visually centred anyway, which is a legitimate
    // disagreement with the designer, decided with the thing on screen.
    //
    // The offset is MEASURED FROM THE ART'S OWN ALPHA at rasterisation time
    // rather than hard-coded, so the three authored-asset rules survive: the
    // renditions stay a drop-in swap, and replacement art with a different
    // balance re-centres itself with no code change. The scan runs only on the
    // rare rebuild (DPI change or hint re-elision), over one markPx-sized
    // image.
    {
        QImage mark(static_cast<int>(markPx), static_cast<int>(markPx),
                    QImage::Format_ARGB32_Premultiplied);
        mark.fill(Qt::transparent);
        QPainter mp(&mark);
        mp.setRenderHint(QPainter::SmoothPixmapTransform, true);
        paintIcon(mp, QRectF(0.0, 0.0, markPx, markPx), QStringLiteral("empty-mark"),
                  {104, 208});
        mp.end();
        // Ink = alpha above a modest floor, so the faint glow does not drag
        // the box; 40 of 255 keeps the measured 59x68 ink and drops the halo.
        int inkL = mark.width(), inkR = -1;
        for (int y = 0; y < mark.height(); ++y) {
            const QRgb* row = reinterpret_cast<const QRgb*>(mark.constScanLine(y));
            for (int x2 = 0; x2 < mark.width(); ++x2) {
                if (qAlpha(row[x2]) >= 40) {
                    if (x2 < inkL) inkL = x2;
                    if (x2 > inkR) inkR = x2;
                }
            }
        }
        const double inkOffset =
            inkR >= inkL ? (inkL + inkR + 1) / 2.0 - markPx / 2.0 : 0.0;
        p.drawImage(QPointF(snap((w - markPx) / 2.0 - inkOffset), 0.0), mark);
    }

    // The CSS line box, followed literally: the gap is measured to the top of
    // the line, and the glyphs sit an ascent below that.
    p.setFont(font);
    p.setPen(QColor(255, 255, 255, kEmptyTextAlpha));
    p.drawText(QPointF(snap((w - textW) / 2.0), markPx + gap + fm.ascent()), shown);
    p.end();

    empty_ = image;
    emptyMarkPx_ = markPx;
    emptyTextCached_ = shown;
    ++emptyRevision_;
}

// THE TWO TIME READOUTS, RASTERISED TOGETHER (UI redesign roadmap step 5).
//
// One image with two halves: the position occupies the left `readoutPx_` and
// the duration the right, each drawn into a cell of the reserved width. Two
// halves of one image rather than two images because they always change
// together -- one rebuild, one revision, one upload per event instead of two.
//
// THE RESERVED WIDTH IS MEASURED FROM THE DURATION, WITH ITS DIGITS REPLACED BY
// EIGHTS, and both halves of that matter. The duration is the widest value the
// position can ever take in the same mode, so reserving it means the track
// never resizes as the playhead moves. Replacing the digits makes the width
// independent of WHICH digits are showing, which a proportional font would
// otherwise change -- and a track that resized on the frame counter would bump
// layoutRevision_ 24 times a second and re-sync the accessibility proxies with
// it.
void OverlayModel::rebuildReadout() {
    const QString position = hooks_.positionText ? hooks_.positionText() : QString();
    const QString duration = hooks_.durationText ? hooks_.durationText() : QString();
    if (position == positionCached_ && duration == durationCached_ && !readout_.isNull())
        return;
    positionCached_ = position;
    durationCached_ = duration;

    if (position.isEmpty() && duration.isEmpty()) {
        if (!readout_.isNull()) {
            readout_ = QImage();
            ++readoutRevision_;
        }
        if (readoutPx_ != 0.0) {
            readoutPx_ = 0.0;
            layout();
        }
        return;
    }

    QFont font;
    font.setPixelSize(std::max(1, static_cast<int>(std::lround(kReadoutTextLogical * dpr_))));
    // TABULAR FIGURES (owner item 9, 2026-08-18): proportional digits have
    // different widths, so every digit change reflowed the string -- 50 -> 51
    // moved the text even with the cell width reserved, which is the counter
    // jitter the owner reported. Segoe UI Variable carries the tnum feature;
    // with it every digit occupies the same advance and a running counter
    // holds still. The eights-reservation below stays, because tnum makes
    // digits equal to EACH OTHER, not to their widest -- the reserved width
    // still has to come from a real string.
#if QT_VERSION >= QT_VERSION_CHECK(6, 7, 0)
    font.setFeature(QFont::Tag("tnum"), 1);
#endif
    const QFontMetrics fm(font);

    QString widest = duration.isEmpty() ? position : duration;
    for (QChar& c : widest) {
        if (c.isDigit()) c = QLatin1Char('8');
    }
    const double cellW = snapTo(fm.horizontalAdvance(widest));
    const double cellH = snapTo(kReadoutTextLogical * dpr_ * 1.4);
    if (cellW <= 0.0 || cellH <= 0.0) return;

    // A width change moves the track, so the layout has to be redone before the
    // rects are used. It is not per-frame: the reserved width is digit-count
    // stable, so this fires on a media change or a readout-mode change and not
    // otherwise.
    if (std::abs(cellW - readoutPx_) >= 0.5) {
        readoutPx_ = cellW;
        layout();
    }

    QImage image(static_cast<int>(cellW * 2), static_cast<int>(cellH),
                 QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return;
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setFont(font);
    // `rgba(255,255,255,0.8)` from the mockup.
    p.setPen(QColor(255, 255, 255, 204));
    // BOTH readouts are LEFT-aligned now (owner item 9, 2026-08-18,
    // superseding the mockup's text-align:right on the position): with the
    // position right-aligned, a digit-COUNT change (99 -> 100) moved its left
    // edge, which read as the whole field shifting. Left-aligned, a growing
    // count extends into the reserved slack toward the track and the value's
    // leading digit holds still -- and tabular figures above hold it still
    // within a count. The track cannot move either way; its position comes
    // from the reserved cell width, not from the text.
    p.drawText(QRectF(0, 0, cellW, cellH), Qt::AlignLeft | Qt::AlignVCenter, position);
    p.drawText(QRectF(cellW, 0, cellW, cellH), Qt::AlignLeft | Qt::AlignVCenter, duration);
    p.end();

    readout_ = image;
    ++readoutRevision_;
}

const std::vector<OverlayQuad>& OverlayModel::buildFrame(QSize surfacePixels) {
    quads_.clear();
    if (surfacePixels.isEmpty()) return quads_;

    // THE EMPTY STATE IS EMITTED BEFORE THE `enabled_` GATE, WHICH IS THE ONE
    // THING IN THIS FUNCTION THAT IS NOT PART OF THE TRANSPORT. It is what the
    // window is when there is no media, so it must not fade with the panel and
    // must not disappear with TRACE_TRANSPORT_BAR=1 -- which is the documented
    // escape hatch and would otherwise be left with a black window and no hint.
    // Same reasoning as the pan gesture being ungated in onMouseDown: a
    // behaviour that belongs to the window rather than to the transport.
    rebuildEmpty(surfacePixels);
    if (!empty_.isNull()) {
        // Snapped, like every other destination rect here, so it is a 1:1 copy
        // on both backends rather than two resamples of the same art.
        // Centred in the stage the chrome leaves, not in the whole surface. See
        // setTopInset: with no media the top chrome is held up, so this is the
        // one state where it is permanently on screen and the difference is
        // permanently visible.
        const QRectF dst(std::floor((surfacePixels.width() - empty_.width()) / 2.0 + 0.5),
                         std::floor(topInset_
                                    + (surfacePixels.height() - topInset_ - empty_.height()) / 2.0
                                    + 0.5),
                         empty_.width(), empty_.height());
        quads_.push_back(OverlayQuad{dst, QRectF(QPointF(0, 0), QSizeF(empty_.size())),
                                     1.0f, 1.0f, OverlayQuad::Source::Empty});
    }

    if (!enabled_) return quads_;

    // The message is deliberately OUTSIDE the opacity gate: a confirmation or
    // an error is shown for its own timeout, whether or not the transport is
    // revealed, and it does not fade with the panel. Top-left with a margin --
    // clear of the bottom-anchored transport at every window shape.
    //
    // BELOW THE TOP CHROME, AND UNCONDITIONALLY SO. Roadmap step 7 turned the
    // menu bar into a strip that floats OVER the picture, and this quad's 12px
    // margin put it entirely behind that strip: measured, the same Ctrl+C draws
    // nothing visible with the chrome revealed and "Copied frame 0 (4096 x
    // 2304)" 300ms later with it hidden. Every route to a message is an input
    // and every input reveals the chrome, so the confirmation was hidden for
    // the first two seconds of its two-and-a-half second life -- present,
    // correct, and unreadable exactly when it fires.
    //
    // The offset is NOT conditional on the chrome being up. It could be: the
    // inset is only non-zero when a strip exists, and the strip's own reveal
    // state is right here in chromeRevealed_. But the auto-hide lands in the
    // middle of a 2.5s message, so a conditional offset would make the toast
    // JUMP 38px partway through being read. A constant position that is
    // sometimes 38px lower than the design's margin is the better of the two,
    // and it is the same reasoning that keeps the message outside the fade.
    rebuildMessage(surfacePixels);
    const auto pushMessage = [&]() {
        if (message_.isNull()) return;
        const double margin = 12.0 * dpr_;
        quads_.push_back(OverlayQuad{QRectF(margin, topInset_ + margin,
                                            message_.width(), message_.height()),
                                     QRectF(QPointF(0, 0), QSizeF(message_.size())),
                                     1.0f, 1.0f, OverlayQuad::Source::Message});
    };

    if (opacity_ <= 0.001) {
        pushMessage();
        return quads_;
    }

    setSurfaceSize(surfacePixels);
    // BEFORE the atlas, because a readout width change re-runs layout() and the
    // atlas cells are sized from what layout() snapped. Rebuilding the atlas
    // first would size its cells against the previous layout on the one frame a
    // media change lands.
    rebuildReadout();
    if (atlasDirty_) rebuildAtlas();
    rebuildText();
    if (atlas_.isNull()) return quads_;

    const float a = static_cast<float>(opacity_);
    const auto hoverBoost = [&](Region r) {
        if (pressed_ == r) return 0.72f;
        if (hover_ == r) return 1.35f;
        return 1.0f;
    };
    const auto push = [&](const QRectF& dst, const QRectF& src, float alpha,
                          float brighten, OverlayQuad::Source source) {
        if (alpha <= 0.001f || dst.width() <= 0.0 || dst.height() <= 0.0) return;
        quads_.push_back(OverlayQuad{dst, src, alpha, brighten, source});
    };

    // The strip: one narrow gradient column stretched across the window. Exact
    // rather than approximate -- see aStrip_.
    push(dStrip_, aStripSample_, a, 1.0f, OverlayQuad::Source::Atlas);

    // THE HOVER PLATE, which is what "radius 6" in the handoff is about: the
    // control's own glyph brightens, and a faint rounded plate appears behind
    // it. Drawn from the white patch, so it costs no atlas cell of its own.
    // Only on hover -- an always-on plate would draw nine boxes across a strip
    // the design shows as flat.
    // The plate cell is chosen by the control's SIZE, so it is exactly the rect
    // it sits behind and the blit stays 1:1 -- a util plate stretched behind the
    // 40px play control would resample its own rounded corners.
    const auto plate = [&](const QRectF& r, Region region) {
        if (hover_ != region && pressed_ != region) return;
        const bool isPlay = r.width() > utilPx_ + 0.5;
        const QRectF& cell = pressed_ == region
            ? (isPlay ? aPlatePlayPressed_ : aPlateUtilPressed_)
            : (isPlay ? aPlatePlay_ : aPlateUtil_);
        push(r, cell, a, 1.0f, OverlayQuad::Source::Atlas);
    };

    const bool playing = hooks_.isPlaying && hooks_.isPlaying();
    const bool muted = hooks_.isMuted && hooks_.isMuted();
    const bool fullscreen = hooks_.isFullscreen && hooks_.isFullscreen();

    // A different source rect, not a different image: this is the state change
    // the caching design exists to make free, and it now serves three toggles
    // rather than one -- play/pause, volume/muted and enter/exit fullscreen.
    const auto control = [&](const QRectF& dst, const QRectF& src, Region region) {
        plate(dst, region);
        push(dst, src, a, hoverBoost(region), OverlayQuad::Source::Atlas);
    };
    control(dRewind_, aRewind_, Region::Rewind);
    control(dPlay_, playing ? aPause_ : aPlay_, Region::PlayPause);
    control(dFfwd_, aFfwd_, Region::FastForward);
    control(dMute_, muted ? aVolumeMuted_ : aVolume_, Region::Mute);
    // LOOP IS THE ONE CONTROL WHOSE STATE IS NOT A SECOND GLYPH, because the
    // package ships ONE loop glyph where it ships a volume/volume-muted pair.
    // On is drawn at the accent; off is the same neutral ink as its neighbours.
    // Two atlas cells would have been the consistent answer and would have meant
    // inventing a second piece of artwork, which is the thing this project does
    // not do quietly -- so the state is carried by colour, and the accent is
    // already the strip's one colour.
    const bool looping = hooks_.isLooping && hooks_.isLooping();
    plate(dLoop_, Region::Loop);
    push(dLoop_, looping ? aLoopOn_ : aLoop_, a, hoverBoost(Region::Loop),
         OverlayQuad::Source::Atlas);
    control(dFullscreen_, fullscreen ? aExitFullscreen_ : aFullscreen_, Region::Fullscreen);
    control(dShare_, aShare_, Region::Share);

    // `rgba(255,255,255,0.14)` from the mockup, baked into its own cell.
    push(dSeparator_, aSeparatorSample_, a, 1.0f, OverlayQuad::Source::Atlas);

    // THE TRACK THICKENS ON HOVER AND THE THUMB GROWS WHILE SCRUBBING, both
    // computed from dTrack_ rather than laid out, so neither moves a control or
    // bumps the layout revision. `dTrack_` is the 4px base; hover expands it
    // about its own centre line to 6.
    const bool trackActive = hover_ == Region::Timeline || draggingTimeline_;
    QRectF track = dTrack_;
    if (trackActive) {
        const double want = std::max(1.0, snapTo(kTrackHoverLogical * dpr_));
        const double grow = snapTo((want - track.height()) / 2.0);
        track = track.adjusted(0, -grow, 0, grow);
    }

    // ACCENT ON THE PLAYED TRACK ONLY, which is the handoff's own rule. The
    // unplayed remainder is white at 0.22 and everything else on the strip is
    // neutral, so the one coloured thing on screen is where the playhead is.
    //
    // ROUND ENDS (owner item 2, 2026-08-18): radius = half the track height.
    // Each run of track is three quads -- a left half-circle cap sliced 1:1
    // from its atlas circle, the stretched middle, a right cap -- because a
    // rounded rect stretched to an arbitrary width would distort its corners
    // and a draw-time rounding does not exist in this pipeline.
    const QRectF& capBg = trackActive ? aTrackCapBgHover_ : aTrackCapBg_;
    const QRectF& capAcc = trackActive ? aTrackCapAccentHover_ : aTrackCapAccent_;
    const double capR = std::min(capBg.width() / 2.0, track.width() / 2.0);
    const auto pushCapped = [&](const QRectF& run, const QRectF& capCell,
                                const QRectF& midSample, bool roundLeft, bool roundRight) {
        double left = run.left();
        double right = run.right();
        if (roundLeft) {
            const double w = std::min(capR, run.width());
            push(QRectF(left, run.top(), w, run.height()),
                 QRectF(capCell.left(), capCell.top(), w, capCell.height()), a, 1.0f,
                 OverlayQuad::Source::Atlas);
            left += w;
        }
        if (roundRight && right - left >= capR) {
            push(QRectF(right - capR, run.top(), capR, run.height()),
                 QRectF(capCell.right() - capR, capCell.top(), capR, capCell.height()), a,
                 1.0f, OverlayQuad::Source::Atlas);
            right -= capR;
        }
        if (right > left)
            push(QRectF(left, run.top(), right - left, run.height()), midSample, a, 1.0f,
                 OverlayQuad::Source::Atlas);
    };
    pushCapped(track, capBg, aTrackBgSample_, true, true);
    const double frac = std::clamp(hooks_.positionFraction ? hooks_.positionFraction() : 0.0,
                                   0.0, 1.0);
    QRectF filled = track;
    filled.setWidth(snapTo(track.width() * frac));
    if (filled.width() > 0.0) {
        // The played portion's right end is square: it either meets the
        // unplayed track mid-run or sits under the thumb at the far end.
        pushCapped(filled, capAcc, aAccentSample_, true,
                   filled.width() >= track.width() - 0.5);
    }

    // Moving the thumb is a destination-rect change only. Snapped like every
    // other rect, so it stays a 1:1 copy as it travels rather than resampling
    // itself differently at each sub-pixel position along the track. ONE cell
    // at one size -- the 16px grow-while-scrubbing variant is removed (owner
    // items 16+17, 2026-08-18): the cell swap mid-drag was the pop.
    const QRectF& thumbCell = aThumb_;
    const QRectF thumbRect(snapTo(track.left() + track.width() * frac
                                  - thumbCell.width() / 2.0),
                           snapTo(track.center().y() - thumbCell.height() / 2.0),
                           thumbCell.width(), thumbCell.height());
    push(thumbRect, thumbCell, a, 1.0f, OverlayQuad::Source::Atlas);

    // The two readouts, each half of one image. Skipped when layout() elided
    // them on a narrow strip, and when there is no media to have a position in.
    if (!readout_.isNull() && readoutPx_ > 0.0) {
        const double halfW = readout_.width() / 2.0;
        if (!dPosition_.isEmpty())
            push(dPosition_, QRectF(0, 0, halfW, readout_.height()), a, 1.0f,
                 OverlayQuad::Source::Readout);
        if (!dDuration_.isEmpty())
            push(dDuration_, QRectF(halfW, 0, halfW, readout_.height()), a, 1.0f,
                 OverlayQuad::Source::Readout);
    }

    if (!text_.isNull()) {
        // CENTRED ABOVE THE STRIP, which is where the approved package puts it
        // (section 6) and where phase 8 recorded that it did not yet sit.
        //
        // It is moved here because the strip's geometry forced it rather than
        // as a separate change: the chip lived at the old panel's top-left, and
        // that panel was 84px tall with an empty corner. A 56px strip has no
        // empty corner -- the chip's ~27px would land on top of Go to Start --
        // so "inside the panel" stopped being a position that exists. The
        // package's own answer was already written down as the thing not yet
        // done, so this takes it rather than inventing a third place.
        //
        // Only the POSITION is the package's here. Its section 6 also specifies
        // the chip's own padding, radius and 900ms/200ms timing, and those stay
        // unimplemented -- the string and its timer are the host's, exactly as
        // they were.
        const QRectF textRect(snapTo((surfacePixels.width() - text_.width()) / 2.0),
                              snapTo(dStrip_.top() - text_.height() - 10 * dpr_),
                              text_.width(), text_.height());
        push(textRect, QRectF(QPointF(0, 0), QSizeF(text_.size())), a, 1.0f,
             OverlayQuad::Source::Text);
    }

    // Last, so it composites over the panel in the one case they could ever
    // meet (a very short window); everywhere else the order is irrelevant.
    pushMessage();

    return quads_;
}

OverlayModel::Region OverlayModel::regionAt(int x, int y) const {
    const QPointF p(x, y);
    if (dPlay_.contains(p)) return Region::PlayPause;
    if (dRewind_.contains(p)) return Region::Rewind;
    if (dFfwd_.contains(p)) return Region::FastForward;
    if (dMute_.contains(p)) return Region::Mute;
    if (dLoop_.contains(p)) return Region::Loop;
    if (dFullscreen_.contains(p)) return Region::Fullscreen;
    if (dShare_.contains(p)) return Region::Share;
    // Last, because its hit rect is the full height of the strip and the
    // buttons' cells touch it -- a track tested first would swallow the row.
    if (trackHitRect().contains(p)) return Region::Timeline;
    return Region::None;
}

double OverlayModel::fractionAt(int x) const {
    return std::clamp((x - dTrack_.left()) / std::max(1.0, dTrack_.width()), 0.0, 1.0);
}

void OverlayModel::reveal(const char* source) {
    if (!enabled_) return;
    if (revealLogEnabled()) {
        fprintf(stderr, "[reveal] %8lld ms  reveal(%s)  opacity %.2f -> target 1.00  hover %d\n",
                static_cast<long long>(revealLogMs()), source ? source : "?", opacity_,
                static_cast<int>(hover_));
        fflush(stderr);
    }
    targetOpacity_ = 1.0;
    startAnimation();
    autoHideTimer_.start();
    // Before the cursor, and before the fade has moved a pixel: the top chrome
    // is a real window that has to be mapped, and mapping it at the START of the
    // fade-in is what makes the two panels arrive together.
    syncChrome();
    // Guarded on the mirrored state rather than called unconditionally: reveal()
    // runs on every pointer move, and this is a cross-boundary call into the
    // host.
    if (cursorHidden_) {
        cursorHidden_ = false;
        if (hooks_.setCursorHidden) hooks_.setCursorHidden(false);
    }
}

bool OverlayModel::onMouseMove(int x, int y) {
    // A MOVE THAT DOES NOT MOVE IS NOT INPUT (owner items 3+7, 2026-08-18).
    // Windows re-evaluates which window is under the cursor whenever any
    // window's visibility changes and posts a synthetic WM_MOUSEMOVE at the
    // UNCHANGED coordinate -- so the top chrome strip (a native window) hiding
    // at the end of its own fade generated a "move", which revealed the
    // chrome, which restarted the idle timer, which hid it again: the strip
    // blinked on a perfect ~2.2s cycle with the pointer parked, windowed and
    // fullscreen alike. Measured with TRACE_REVEAL_LOG=1: every `chrome
    // HIDDEN` was followed within 2-5ms by a mousemove at the identical pixel,
    // 46 of them across a run with zero physical pointer motion. Filtering on
    // "did the coordinate change" removes the synthetic class exactly, because
    // a real gesture cannot arrive at the pixel it is already on.
    const bool moved = x != lastMoveX_ || y != lastMoveY_;
    lastMoveX_ = x;
    lastMoveY_ = y;

    // Before the enabled_ gate, because a pan can be in progress with the
    // docked bar selected. The delta is taken here rather than by the host, so
    // there is one drag anchor in the application rather than two.
    if (panning_) {
        if (hooks_.panBy) hooks_.panBy(x - panLastX_, y - panLastY_);
        panLastX_ = x;
        panLastY_ = y;
        if (enabled_ && moved) reveal("mousemove-pan");
        return true;
    }
    if (!enabled_) return false;
    if (!moved) {
        if (revealLogEnabled()) {
            fprintf(stderr, "[reveal] %8lld ms  mousemove %d,%d SYNTHETIC (filtered)\n",
                    static_cast<long long>(revealLogMs()), x, y);
            fflush(stderr);
        }
        return hover_ != Region::None;
    }
    if (revealLogEnabled()) {
        fprintf(stderr, "[reveal] %8lld ms  mousemove %d,%d\n",
                static_cast<long long>(revealLogMs()), x, y);
        fflush(stderr);
    }
    reveal("mousemove");

    if (draggingTimeline_) {
        if (hooks_.seekToFraction) hooks_.seekToFraction(fractionAt(x));
        if (hooks_.requestRepaint) hooks_.requestRepaint();
        return true;
    }

    const Region was = hover_;
    hover_ = visible() ? regionAt(x, y) : Region::None;
    if (was != hover_ && hooks_.requestRepaint) hooks_.requestRepaint();
    return hover_ != Region::None;
}

bool OverlayModel::onMouseDown(int x, int y) {
    if (enabled_) {
        // "Clicking the video reveals it" -- so the reveal happens before the
        // visibility test, and a click on a hidden overlay brings it back rather
        // than pressing a control the user cannot see. The press is NOT also
        // delivered to that control: a click has to land on something visible.
        const bool wasVisible = visible();
        reveal("mousedown");
        if (wasVisible) {
            const Region r = regionAt(x, y);
            if (r != Region::None) {
                pressed_ = r;
                if (r == Region::Timeline) {
                    draggingTimeline_ = true;
                    // Press first, then position: this is the slider's own
                    // order, and going through it is what gives the overlay the
                    // real scrub path rather than a second one.
                    if (hooks_.setScrubbing) hooks_.setScrubbing(true);
                    if (hooks_.seekToFraction) hooks_.seekToFraction(fractionAt(x));
                }
                if (hooks_.requestRepaint) hooks_.requestRepaint();
                return true;
            }
        }
    }
    // A PRESS ON THE STRIP'S OWN BACKGROUND IS CONSUMED, AND THE EDGE-TO-EDGE
    // SHAPE IS WHAT MADE THAT NECESSARY.
    //
    // Below, a press that lands on no control starts a PAN when the picture is
    // zoomed. That was harmless while the transport was a 460px box floating
    // clear of most of the window -- the gap between its controls is a few
    // pixels wide. The strip spans the whole window, so the space between the
    // button cluster and the readout, and the whole right end past Share, are
    // now large areas of transport that would have dragged the picture out from
    // under the strip the user was aiming at.
    //
    // Gated on the strip being VISIBLE rather than merely laid out: with the
    // panel faded out there is nothing on screen to have pressed, and a press
    // there is a press on the picture like any other.
    if (enabled_ && visible() && dStrip_.contains(QPointF(x, y))) return true;

    // Not a control, or there is no overlay at all. A press on the PICTURE, and
    // spec phase 15 gives that a meaning when the picture is bigger than the
    // viewport. Reached with the overlay disabled too, deliberately: panning is
    // a view gesture and must not disappear with TRACE_TRANSPORT_BAR=1, exactly
    // as double-click-to-fullscreen does not.
    if (hooks_.canPan && hooks_.canPan()) {
        panning_ = true;
        panLastX_ = x;
        panLastY_ = y;
        return true;
    }
    return false;
}

bool OverlayModel::onMouseUp(int x, int y) {
    if (panning_) {
        panning_ = false;
        return true;
    }
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
            // Both side regions are shuttle controls now, and the artwork says
            // so. The names, the glyphs and the actions moved together in the
            // commit that changed each one -- phase 4 forward, phase 5 backward
            // -- because renaming a hook without changing what it calls would
            // put a lie in the contract, which is the specific trap
            // `isVideoScrubActive()` set.
            case Region::Rewind:      if (hooks_.rewind) hooks_.rewind(); break;
            case Region::FastForward: if (hooks_.fastForward) hooks_.fastForward(); break;
            // UI redesign roadmap step 5. Each is one call to an action the
            // host already owns -- two exact seeks and a mute toggle -- so the
            // overlay still issues no command of its own and still holds no
            // playback state that could drift.
            case Region::Mute:        if (hooks_.toggleMute) hooks_.toggleMute(); break;
            case Region::Loop:        if (hooks_.toggleLoop) hooks_.toggleLoop(); break;
            // The SAME action the double-click and F11 run, which is why this
            // needs no new hook: toggleFullscreen has been in this struct since
            // spec phase 6, because the D3D11 surface takes every mouse message
            // and a second route would have to reimplement that plumbing.
            case Region::Fullscreen:  if (hooks_.toggleFullscreen) hooks_.toggleFullscreen();
                                      break;
            // Spec phase 8. The point is passed in surface device pixels and
            // the host converts; the overlay does not know what a screen
            // coordinate is on either backend.
            case Region::Share:       if (hooks_.shareMenu) hooks_.shareMenu(
                                          static_cast<int>(dShare_.left()),
                                          static_cast<int>(dShare_.bottom())); break;
            default: break;
        }
        if (hooks_.requestRepaint) hooks_.requestRepaint();
        return true;
    }
    if (hooks_.requestRepaint) hooks_.requestRepaint();
    return was != Region::None;
}

bool OverlayModel::onMouseDoubleClick(int x, int y) {
    // Deliberately NOT gated on enabled_: with the docked bar selected there is
    // no overlay to consult, and double-click-to-fullscreen is a spec'd window
    // gesture that must not disappear with the transport it is unrelated to.
    if (enabled_) {
        const bool wasVisible = visible();
        reveal("dblclick");
        // A double-click on a control is THAT CONTROL BEING PRESSED AGAIN, not a
        // request to change the window -- so it is forwarded as a press rather
        // than swallowed, and the release that follows turns it into a click the
        // same way any other press does.
        //
        // Windows sends down, up, DBLCLK, up: the second press of a rapid pair
        // arrives as WM_LBUTTONDBLCLK and NOT as WM_LBUTTONDOWN. Consuming it
        // would drop every other press inside the double-click interval, which
        // is precisely the gesture the shuttle ladder is specified by -- six
        // rapid presses of Fast-forward must reach 30x, and after phase 6 the
        // overlay's is the only Fast-forward there is. Qt's own
        // QWidget::mouseDoubleClickEvent forwards to mousePressEvent for the
        // same reason, which is why the docked bar's buttons never had this
        // problem and why the fault would have looked like an overlay-only
        // ladder bug.
        if (wasVisible && regionAt(x, y) != Region::None) return onMouseDown(x, y);
        // And a double-click on the strip's own background is NOT a request to
        // change the window, for the same reason a single press there is not a
        // pan: the strip is interface, not picture. Without this, a stray
        // double-click anywhere along a full-width transport toggles fullscreen.
        if (wasVisible && dStrip_.contains(QPointF(x, y))) return true;
    }
    if (hooks_.toggleFullscreen) {
        hooks_.toggleFullscreen();
        return true;
    }
    return false;
}

void OverlayModel::onMouseLeave() {
    // Forget the last coordinate: a genuine re-entry must always reveal, even
    // in the unlikely case it lands on the exact pixel the pointer left from.
    lastMoveX_ = INT_MIN;
    lastMoveY_ = INT_MIN;
    if (hover_ == Region::None && pressed_ == Region::None) return;
    hover_ = Region::None;
    if (!draggingTimeline_) pressed_ = Region::None;
    autoHideTimer_.start();
    if (hooks_.requestRepaint) hooks_.requestRepaint();
}

void OverlayModel::startAnimation() {
    if (std::abs(targetOpacity_ - opacity_) < 0.001) return;
    if (!fadeClock_.isValid()) fadeClock_.start();
    if (!animTimer_.isActive()) {
        fadeClock_.restart();
        animTimer_.start();
    }
}

void OverlayModel::tickAnimation() {
    const double step = static_cast<double>(fadeClock_.restart()) / kFadeMs;
    if (opacity_ < targetOpacity_) opacity_ = std::min(targetOpacity_, opacity_ + step);
    else opacity_ = std::max(targetOpacity_, opacity_ - step);

    if (std::abs(targetOpacity_ - opacity_) < 0.001) {
        opacity_ = targetOpacity_;
        animTimer_.stop();
        if (opacity_ <= 0.0) hover_ = Region::None;
    }
    // Before syncChrome, so when a fade-out lands on 0 the strip's alpha is
    // already 0 by the time the revealed=false edge unmaps it.
    if (hooks_.setChromeOpacity) hooks_.setChromeOpacity(opacity_);
    syncChrome();
    if (hooks_.requestRepaint) hooks_.requestRepaint();
}

// ONE reveal state, read the same way for both panels. "Revealed" is true from
// the moment a fade-in is asked for until a fade-out has finished, which is what
// makes the strip appear with the transport and leave with it rather than
// blinking out halfway down the fade.
//
// It cannot be `visible()` alone: opacity_ is still 0 when reveal() sets the
// target, so a strip driven off that would arrive 16ms late on every reveal.
void OverlayModel::syncChrome() {
    const bool revealed = targetOpacity_ > 0.001 || opacity_ > 0.001;
    if (revealed == chromeRevealed_) return;
    chromeRevealed_ = revealed;
    if (revealLogEnabled()) {
        fprintf(stderr, "[reveal] %8lld ms  chrome %s\n",
                static_cast<long long>(revealLogMs()), revealed ? "SHOWN" : "HIDDEN");
        fflush(stderr);
    }
    if (hooks_.setChromeRevealed) hooks_.setChromeRevealed(revealed);
}

} // namespace trace::render
