#pragma once

#include <QImage>
#include <QString>

#include "render/VideoRenderer.h"

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
    QString name() const override { return QStringLiteral("cpu"); }
    const RenderStats& stats() const override { return stats_; }

private:
    trace::core::VideoFrame frame_;
    // Built once per frame rather than per paint. The view keeps the frame's
    // buffer alive on its own, so it stays valid even if frame_ is replaced.
    QImage image_;
    bool hasImage_ = false;
    QString placeholder_ = QStringLiteral("Drop media or File > Open");
    RenderStats stats_{};
};

} // namespace trace::render
