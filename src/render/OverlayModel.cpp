#include "render/OverlayModel.h"

#include <QBrush>
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QFont>
#include <QFontMetrics>
#include <QHash>
#include <QIcon>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPixmap>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace trace::render {
namespace {

// Fade duration, inside the 150-180ms band asked for. One constant, used for
// both directions, so appearing and disappearing feel symmetrical.
constexpr int kFadeMs = 165;
// Idle time before the overlay hides itself again. The spec's provisional
// figure.
constexpr int kAutoHideMs = 2000;
// Animation tick. 16ms is one frame at 60Hz; the timer only runs while
// something is actually animating, which is what keeps a hidden overlay free.
constexpr int kAnimTickMs = 16;

// The approved package's control geometry, adopted at spec phase 6. Phase 2
// deliberately declined to apply it to the docked bar, because this is the
// geometry of the FLOATING transport and re-laying-out a widget phase 6 removes
// from the layout would have moved the video rect for a component with no
// future. This is that component.
constexpr double kPanelWidthLogical = 460.0;
constexpr double kPanelHeightLogical = 84.0;
constexpr double kPanelMarginLogical = 28.0;
constexpr double kPlayLogical = 44.0;
constexpr double kUtilLogical = 34.0;
constexpr double kHandleLogical = 16.0;

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
            autoHideTimer_.start();
            return;
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

    const double panelW = snap(std::min<double>(kPanelWidthLogical * s,
                                                surfaceSize_.width() * 0.9));
    const double panelH = snap(kPanelHeightLogical * s);
    const double margin = snap(kPanelMarginLogical * s);
    const double left = snap((surfaceSize_.width() - panelW) / 2.0);
    const double top = snap(surfaceSize_.height() - panelH - margin);
    dPanel_ = QRectF(left, top, panelW, panelH);

    playPx_ = snap(kPlayLogical * s);
    utilPx_ = snap(kUtilLogical * s);
    handlePx_ = snap(kHandleLogical * s);

    // The three controls sit on one centre line and differ in size, so each
    // rect is derived from that line rather than from a shared top edge --
    // aligning their tops would put the two 34px glyphs 5px above the 44px one.
    const double rowCy = top + panelH * 0.36;
    const double cx = left + panelW / 2.0;
    // Centre-to-centre: half of each control plus a constant clear gap, so the
    // visual spacing stays even when the two sizes differ.
    const double gap = snap((playPx_ + utilPx_) / 2.0 + 22.0 * s);
    dPlay_ = QRectF(snap(cx - playPx_ / 2.0), snap(rowCy - playPx_ / 2.0), playPx_, playPx_);
    dRewind_ = QRectF(snap(cx - gap - utilPx_ / 2.0), snap(rowCy - utilPx_ / 2.0),
                      utilPx_, utilPx_);
    dFfwd_ = QRectF(snap(cx + gap - utilPx_ / 2.0), snap(rowCy - utilPx_ / 2.0),
                    utilPx_, utilPx_);

    // Spec phase 8, and it fits INSIDE the settled panel rather than growing it.
    // kPanelWidthLogical, kPanelHeightLogical and the 44/34 control sizes are
    // owner-signed-off numbers as of phase 6 -- changing one reopens a decision
    // rather than tuning a constant -- and the three centred controls only reach
    // 78 logical px either side of centre, so the right end of the row was
    // already empty. That is also where the design package puts it: the top row
    // is "rewind - play/pause - forward | fullscreen - share".
    const double edgeInset = snap(16.0 * s);
    dShare_ = QRectF(snap(dPanel_.right() - edgeInset - utilPx_),
                     snap(rowCy - utilPx_ / 2.0), utilPx_, utilPx_);

    const double trackInset = snap(24.0 * s);
    const double trackY = snap(top + panelH * 0.76);
    dTrack_ = QRectF(left + trackInset, snap(trackY - 2.0 * s),
                     panelW - trackInset * 2.0, std::max(1.0, snap(4.0 * s)));

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
    if (dPanel_.isEmpty()) return rects;
    rects.reserve(5);
    rects.push_back({Region::Rewind, dRewind_});
    rects.push_back({Region::PlayPause, dPlay_});
    rects.push_back({Region::FastForward, dFfwd_});
    rects.push_back({Region::Share, dShare_});
    rects.push_back({Region::Timeline, trackHitRect()});
    return rects;
}

// Generous vertical band around the track, because a 4px line is not a pointer
// target. ONE definition, asked by the hit test and by the accessibility
// proxies alike.
QRectF OverlayModel::trackHitRect() const {
    return dTrack_.adjusted(-8 * dpr_, -12 * dpr_, 8 * dpr_, 12 * dpr_);
}

void OverlayModel::rebuildAtlas() {
    const double s = dpr_;
    // The SAME snapped sizes layout() placed, not the constants recomputed --
    // an atlas cell one pixel off from its destination rect would reintroduce
    // the resample the snapping exists to remove. Two icon sizes since spec
    // phase 6, and the row is as tall as the larger of them.
    const double play = playPx_;
    const double util = utilPx_;
    const double rowH = std::max(play, util);
    const double handle = handlePx_;
    const double panelW = dPanel_.width();
    const double panelH = dPanel_.height();

    // Layout the atlas: panel on top, then a row of icons.
    // play x2 (play, pause), util x3 (rewind, fast-forward, share), the handle,
    // the 8px solid patch, and the 4px gaps between them. Undercounting here
    // silently clips the LAST cell, which is why the count is spelled out.
    const int atlasW = static_cast<int>(
        std::ceil(std::max(panelW, play * 2 + util * 3 + handle + 40)));
    const int atlasH = static_cast<int>(std::ceil(panelH + rowH + 16));
    if (atlasW <= 0 || atlasH <= 0) return;

    QImage image(atlasW, atlasH, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) return;
    image.fill(Qt::transparent);
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);

    aPanel_ = QRectF(0, 0, panelW, panelH);
    QPainterPath panel;
    panel.addRoundedRect(aPanel_.adjusted(0.5, 0.5, -0.5, -0.5), 10.0 * s, 10.0 * s);
    // Translucent, which is the whole reason this is composited into the render
    // pass rather than stacked as a native window.
    p.fillPath(panel, QColor(18, 18, 20, 165));
    p.setPen(QPen(QColor(255, 255, 255, 40), 1.0));
    p.drawPath(panel);

    const double row = panelH + 8;
    double x = 4;
    aPlay_ = QRectF(x, row, play, play);   paintIcon(p, aPlay_, "play");        x += play + 4;
    aPause_ = QRectF(x, row, play, play);  paintIcon(p, aPause_, "pause");      x += play + 4;
    // Artwork follows behaviour, one control at a time -- and as of spec phase 5
    // both of them have moved. The right region became Fast-forward at phase 4
    // and the left became Rewind here, so both carry the continuous-scan glyphs
    // and neither frame-step glyph is in the tree any more.
    aRewind_ = QRectF(x, row, util, util); paintIcon(p, aRewind_, "rewind");       x += util + 4;
    aFfwd_ = QRectF(x, row, util, util);   paintIcon(p, aFfwd_, "fast-forward");   x += util + 4;
    aShare_ = QRectF(x, row, util, util);  paintIcon(p, aShare_, "share");         x += util + 4;

    aHandle_ = QRectF(x, row, handle, handle);
    p.setBrush(QColor(255, 255, 255));
    p.setPen(Qt::NoPen);
    p.drawEllipse(aHandle_.adjusted(1, 1, -1, -1));
    x += handle + 4;

    // A plain white patch, tinted at draw time. Lets the timeline track and its
    // filled portion be two draws of one texel rather than two more images.
    //
    // Drawn 8x8 and SAMPLED from its inner 6x6. This is the one quad that is
    // genuinely stretched -- a long thin track from a small square -- so both
    // backends filter it, and a source rect flush with the patch edge would
    // pull the surrounding transparency in and fringe the ends of the track.
    // Insetting is cheaper and more obvious than clamping in two places.
    aSolid_ = QRectF(x, row, 8, 8);
    p.fillRect(aSolid_, QColor(255, 255, 255));
    aSolidSample_ = aSolid_.adjusted(1, 1, -1, -1);

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

    // The mark's own canvas is square and its art sits off-centre inside it by
    // design -- a right-pointing triangle is balanced by eye, not by its
    // bounding box -- so the CANVAS is what gets centred, and the offset comes
    // along. Measured against the delivered mockup: 9.5px of a 103px canvas,
    // reproduced to the pixel.
    paintIcon(p, QRectF(snap((w - markPx) / 2.0), 0.0, markPx, markPx),
              QStringLiteral("empty-mark"), {104, 208});

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
        const QRectF dst(std::floor((surfacePixels.width() - empty_.width()) / 2.0 + 0.5),
                         std::floor((surfacePixels.height() - empty_.height()) / 2.0 + 0.5),
                         empty_.width(), empty_.height());
        quads_.push_back(OverlayQuad{dst, QRectF(QPointF(0, 0), QSizeF(empty_.size())),
                                     1.0f, 1.0f, OverlayQuad::Source::Empty});
    }

    if (!enabled_) return quads_;

    // The message is deliberately OUTSIDE the opacity gate: a confirmation or
    // an error is shown for its own timeout, whether or not the transport is
    // revealed, and it does not fade with the panel. Top-left with a margin --
    // clear of the bottom-centred panel at every window shape, including the
    // 460px-wide portrait minimum.
    rebuildMessage(surfacePixels);
    const auto pushMessage = [&]() {
        if (message_.isNull()) return;
        const double margin = 12.0 * dpr_;
        quads_.push_back(OverlayQuad{QRectF(margin, margin, message_.width(), message_.height()),
                                     QRectF(QPointF(0, 0), QSizeF(message_.size())),
                                     1.0f, 1.0f, OverlayQuad::Source::Message});
    };

    if (opacity_ <= 0.001) {
        pushMessage();
        return quads_;
    }

    setSurfaceSize(surfacePixels);
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

    push(dPanel_, aPanel_, a, 1.0f, OverlayQuad::Source::Atlas);

    const bool playing = hooks_.isPlaying && hooks_.isPlaying();
    // A different source rect, not a different image: this is the state change
    // the caching design exists to make free.
    push(dPlay_, playing ? aPause_ : aPlay_, a, hoverBoost(Region::PlayPause),
         OverlayQuad::Source::Atlas);
    push(dRewind_, aRewind_, a, hoverBoost(Region::Rewind), OverlayQuad::Source::Atlas);
    push(dFfwd_, aFfwd_, a, hoverBoost(Region::FastForward), OverlayQuad::Source::Atlas);
    push(dShare_, aShare_, a, hoverBoost(Region::Share), OverlayQuad::Source::Atlas);

    // Track, filled portion, then handle. All three are the same white patch
    // tinted differently, so none of them can invalidate the atlas.
    push(dTrack_, aSolidSample_, a * 0.35f, 1.0f, OverlayQuad::Source::Atlas);
    const double frac = std::clamp(hooks_.positionFraction ? hooks_.positionFraction() : 0.0,
                                   0.0, 1.0);
    QRectF filled = dTrack_;
    filled.setWidth(dTrack_.width() * frac);
    push(filled, aSolidSample_, a * 0.85f, 1.0f, OverlayQuad::Source::Atlas);

    // Moving the handle is a destination-rect change only. Snapped like every
    // other rect, so it stays a 1:1 copy as it travels rather than resampling
    // itself differently at each sub-pixel position along the track.
    const double handleD = handlePx_;
    const QRectF handleRect(std::floor(dTrack_.left() + dTrack_.width() * frac
                                       - handleD / 2.0 + 0.5),
                            std::floor(dTrack_.center().y() - handleD / 2.0 + 0.5),
                            handleD, handleD);
    push(handleRect, aHandle_, a,
         hover_ == Region::Timeline || draggingTimeline_ ? 1.0f : 0.85f,
         OverlayQuad::Source::Atlas);

    if (!text_.isNull()) {
        // TOP-LEFT since spec phase 8, and this is the one thing the Share
        // button did move. The chip used to sit at the panel's top-right, which
        // is where the Share control now is: at 84px of panel height the chip
        // spans y 10..31 and a 34px control centred on the row spans 13..47, so
        // they overlapped outright. Left is the smallest change that resolves
        // it -- the panel, the controls, the fade and the auto-hide are all
        // untouched, and the chip stays inside the panel where it has been seen.
        //
        // Note the approved package actually specifies the rate chip CENTRED
        // ABOVE the transport rather than inside it (section 6, with its own
        // padding, radius and 900ms/200ms timing). That remains unimplemented
        // and is not this phase's to change.
        const QRectF textRect(dPanel_.left() + 14 * dpr_,
                              dPanel_.top() + 10 * dpr_,
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
    const QRectF trackHit = trackHitRect();
    if (dPlay_.contains(p)) return Region::PlayPause;
    if (dRewind_.contains(p)) return Region::Rewind;
    if (dFfwd_.contains(p)) return Region::FastForward;
    if (dShare_.contains(p)) return Region::Share;
    if (trackHit.contains(p)) return Region::Timeline;
    return Region::None;
}

double OverlayModel::fractionAt(int x) const {
    return std::clamp((x - dTrack_.left()) / std::max(1.0, dTrack_.width()), 0.0, 1.0);
}

void OverlayModel::reveal() {
    if (!enabled_) return;
    targetOpacity_ = 1.0;
    startAnimation();
    autoHideTimer_.start();
    // Guarded on the mirrored state rather than called unconditionally: reveal()
    // runs on every pointer move, and this is a cross-boundary call into the
    // host.
    if (cursorHidden_) {
        cursorHidden_ = false;
        if (hooks_.setCursorHidden) hooks_.setCursorHidden(false);
    }
}

bool OverlayModel::onMouseMove(int x, int y) {
    // Before the enabled_ gate, because a pan can be in progress with the
    // docked bar selected. The delta is taken here rather than by the host, so
    // there is one drag anchor in the application rather than two.
    if (panning_) {
        if (hooks_.panBy) hooks_.panBy(x - panLastX_, y - panLastY_);
        panLastX_ = x;
        panLastY_ = y;
        if (enabled_) reveal();
        return true;
    }
    if (!enabled_) return false;
    reveal();

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
        reveal();
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
        reveal();
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
    }
    if (hooks_.toggleFullscreen) {
        hooks_.toggleFullscreen();
        return true;
    }
    return false;
}

void OverlayModel::onMouseLeave() {
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
    if (hooks_.requestRepaint) hooks_.requestRepaint();
}

} // namespace trace::render
