#pragma once

#include <QImage>
#include <QString>

#include <vector>

#include "render/OverlayModel.h"
#include "render/VideoRenderer.h"

QT_BEGIN_NAMESPACE
class QPainter;
QT_END_NAMESPACE

namespace trace::render {

// The shipped path: swscale has already produced BGRA, so presenting is a
// QPainter blit of a zero-copy view over the frame's buffer, scaled to fit.
//
// This is the reference implementation in the strict sense -- it is the one
// with a measured baseline, and any other backend is judged against what it
// puts on screen.
class CpuImageRenderer final : public VideoRenderer {
public:
    bool initialize(QWidget* host, QString& error) override;
    void setFrame(const trace::core::VideoFrame& frame) override;
    void clearFrame() override;
    void setPlaceholderText(const QString& text) override;
    void resize(QSize size) override;
    void paint(QWidget* host) override;
    void setOverlay(OverlayModel* model) override { overlayModel_ = model; }
    QString name() const override { return QStringLiteral("cpu"); }
    const RenderStats& stats() const override { return stats_; }

private:
    void paintOverlay(QPainter& p, QWidget* host);
    // The atlas with its RGB scaled by `brighten` and its alpha left alone --
    // which is exactly what the D3D11 tint does, and the only part of a quad
    // QPainter cannot express directly.
    //
    // Cached rather than recomputed, and invalidated by the model's atlas
    // REVISION, so it inherits the same "only three things rebuild this"
    // guarantee the atlas itself has. There are three brighten values in the
    // design (rest, hover, press), so this is three small images at most and
    // nothing in the paint path allocates once they exist.
    const QImage& tintedAtlas(const QImage& atlas, long long revision, float brighten);

    trace::core::VideoFrame frame_;
    // Built once per frame rather than per paint. The view keeps the frame's
    // buffer alive on its own, so it stays valid even if frame_ is replaced.
    QImage image_;
    bool hasImage_ = false;
    QString placeholder_ = QStringLiteral("Drop media or File > Open");
    RenderStats stats_{};

    // Non-owning; the host owns it. Null when there is no overlay.
    OverlayModel* overlayModel_ = nullptr;
    struct TintCache {
        float brighten = 1.0f;
        long long revision = -1;
        QImage image;
    };
    std::vector<TintCache> tints_;
};

} // namespace trace::render
