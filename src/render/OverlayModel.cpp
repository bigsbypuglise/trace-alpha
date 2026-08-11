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

constexpr double kPanelWidthLogical = 460.0;
constexpr double kPanelHeightLogical = 76.0;
constexpr double kPanelMarginLogical = 28.0;
constexpr double kIconLogical = 30.0;
constexpr double kHandleLogical = 16.0;

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
void paintIcon(QPainter& p, const QRectF& r, const QString& baseName) {
    // Cached by base name: an atlas rebuild is rare, but a QIcon assembled from
    // resources on each one is avoidable work for no benefit.
    static QHash<QString, QIcon> cache;
    auto it = cache.find(baseName);
    if (it == cache.end()) {
        QIcon icon;
        icon.addFile(QStringLiteral(":/ui/%1-24.png").arg(baseName), QSize(24, 24));
        icon.addFile(QStringLiteral(":/ui/%1-48.png").arg(baseName), QSize(48, 48));
        const QString at3x = QStringLiteral(":/ui/%1-72.png").arg(baseName);
        if (QFile::exists(at3x)) icon.addFile(at3x, QSize(72, 72));
        it = cache.insert(baseName, icon);
    }

    const QSize cell(std::max(1, static_cast<int>(std::lround(r.width()))),
                     std::max(1, static_cast<int>(std::lround(r.height()))));
    const QPixmap pm = it.value().pixmap(cell);
    if (pm.isNull()) return;
    p.drawPixmap(r.topLeft(), pm);
}

} // namespace

// Two names are accepted on purpose. TRACE_OVERLAY is what this is now -- a
// renderer-neutral overlay, not a D3D11 compositing trick -- and
// TRACE_OVERLAY_COMPOSITED is retained because the measurement harness
// (`overlay.ps1`, `overlay_cost.ps1`) sets it, and silently breaking the
// instrument that has to prove this change is free would be the wrong kind of
// tidy.
bool OverlayModel::enabledByEnvironment() {
    static const bool on = [] {
        const QByteArray a = qgetenv("TRACE_OVERLAY");
        if (!a.isEmpty()) return a != "0";
        const QByteArray b = qgetenv("TRACE_OVERLAY_COMPOSITED");
        return !b.isEmpty() && b != "0";
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
        // while being used is worse than one that never appears.
        if (hover_ != Region::None || draggingTimeline_) { autoHideTimer_.start(); return; }
        targetOpacity_ = 0.0;
        startAnimation();
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

    iconPx_ = snap(kIconLogical * s);
    handlePx_ = snap(kHandleLogical * s);
    const double iconY = snap(top + (panelH * 0.30) - iconPx_ / 2.0);
    const double cx = left + panelW / 2.0;
    const double gap = snap(iconPx_ * 1.9);
    dPlay_ = QRectF(snap(cx - iconPx_ / 2.0), iconY, iconPx_, iconPx_);
    dRewind_ = QRectF(snap(cx - gap - iconPx_ / 2.0), iconY, iconPx_, iconPx_);
    dFfwd_ = QRectF(snap(cx + gap - iconPx_ / 2.0), iconY, iconPx_, iconPx_);

    const double trackInset = snap(24.0 * s);
    const double trackY = snap(top + panelH * 0.72);
    dTrack_ = QRectF(left + trackInset, snap(trackY - 2.0 * s),
                     panelW - trackInset * 2.0, std::max(1.0, snap(4.0 * s)));
}

void OverlayModel::rebuildAtlas() {
    const double s = dpr_;
    // The SAME snapped sizes layout() placed, not the constants recomputed --
    // an atlas cell one pixel off from its destination rect would reintroduce
    // the resample the snapping exists to remove.
    const double icon = iconPx_;
    const double handle = handlePx_;
    const double panelW = dPanel_.width();
    const double panelH = dPanel_.height();

    // Layout the atlas: panel on top, then a row of icons.
    const int atlasW = static_cast<int>(std::ceil(std::max(panelW, icon * 4 + handle + 40)));
    const int atlasH = static_cast<int>(std::ceil(panelH + icon + 16));
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
    aPlay_ = QRectF(x, row, icon, icon);   paintIcon(p, aPlay_, "play");        x += icon + 4;
    aPause_ = QRectF(x, row, icon, icon);  paintIcon(p, aPause_, "pause");      x += icon + 4;
    // Artwork follows behaviour, one control at a time -- and as of spec phase 5
    // both of them have moved. The right region became Fast-forward at phase 4
    // and the left became Rewind here, so both carry the continuous-scan glyphs
    // and neither frame-step glyph is in the tree any more.
    aRewind_ = QRectF(x, row, icon, icon); paintIcon(p, aRewind_, "rewind");       x += icon + 4;
    aFfwd_ = QRectF(x, row, icon, icon);   paintIcon(p, aFfwd_, "fast-forward");   x += icon + 4;

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

const std::vector<OverlayQuad>& OverlayModel::buildFrame(QSize surfacePixels) {
    quads_.clear();
    if (!enabled_ || surfacePixels.isEmpty()) return quads_;
    if (opacity_ <= 0.001) return quads_;

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
        const QRectF textRect(dPanel_.right() - text_.width() - 14 * dpr_,
                              dPanel_.top() + 10 * dpr_,
                              text_.width(), text_.height());
        push(textRect, QRectF(QPointF(0, 0), QSizeF(text_.size())), a, 1.0f,
             OverlayQuad::Source::Text);
    }

    return quads_;
}

OverlayModel::Region OverlayModel::regionAt(int x, int y) const {
    const QPointF p(x, y);
    // Generous vertical band around the track, because a 4px line is not a
    // pointer target.
    const QRectF trackHit = dTrack_.adjusted(-8 * dpr_, -12 * dpr_, 8 * dpr_, 12 * dpr_);
    if (dPlay_.contains(p)) return Region::PlayPause;
    if (dRewind_.contains(p)) return Region::Rewind;
    if (dFfwd_.contains(p)) return Region::FastForward;
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
}

bool OverlayModel::onMouseMove(int x, int y) {
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
    if (!enabled_ || !visible()) return false;
    const Region r = regionAt(x, y);
    if (r == Region::None) return false;

    pressed_ = r;
    if (r == Region::Timeline) {
        draggingTimeline_ = true;
        // Press first, then position: this is the slider's own order, and going
        // through it is what gives the overlay the real scrub path rather than
        // a second one.
        if (hooks_.setScrubbing) hooks_.setScrubbing(true);
        if (hooks_.seekToFraction) hooks_.seekToFraction(fractionAt(x));
    }
    if (hooks_.requestRepaint) hooks_.requestRepaint();
    return true;
}

bool OverlayModel::onMouseUp(int x, int y) {
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
            default: break;
        }
        if (hooks_.requestRepaint) hooks_.requestRepaint();
        return true;
    }
    if (hooks_.requestRepaint) hooks_.requestRepaint();
    return was != Region::None;
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
