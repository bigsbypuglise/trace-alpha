#pragma once

#include <QWidget>
#include <QIcon>
#include <QString>

QT_BEGIN_NAMESPACE
class QSlider;
class QLabel;
class QHBoxLayout;
QT_END_NAMESPACE

namespace trace::ui {

// A single transport control drawn to the design spec:
//   44x44 hit target (compact 40x40), radius 10 (compact 9)
//   rest 0.78 / hover 1.00 + bg 0.08 / pressed 1.00 + bg 0.15
//   active 1.00 + bg 0.12 + inner stroke 0.22 / disabled 0.26
//   focus ring 2px #6AA6FF
//
// Qt's stylesheet system can't express per-state icon opacity cleanly, so
// the button paints itself. Icons are white-on-transparent PNGs; opacity is
// the only tint needed on a dark stage.
class TransportButton final : public QWidget {
    Q_OBJECT
public:
    explicit TransportButton(QWidget* parent = nullptr);

    void setIcon(const QIcon& icon);
    void setCenterControl(bool center);
    void setActive(bool active);
    bool isActive() const { return active_; }
    void setEnabledControl(bool enabled);

signals:
    void clicked();

protected:
    void paintEvent(QPaintEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    QSize sizeHint() const override;

private:
    QIcon icon_;
    bool center_ = false;
    bool hovered_ = false;
    bool pressed_ = false;
    bool active_ = false;
};

// Bottom transport bar. Deliberately limited to controls the app actually
// implements — play/pause, frame stepping, fullscreen — plus the timeline.
class TransportBar final : public QWidget {
    Q_OBJECT
public:
    explicit TransportBar(QWidget* parent = nullptr);

    QSlider* timelineSlider() const { return slider_; }

    void setPlaying(bool playing);
    void setFullscreen(bool fullscreen);
    void setFrameText(const QString& text);
    void setControlsEnabled(bool enabled);

signals:
    void prevFrameClicked();
    void playPauseClicked();
    void nextFrameClicked();
    void fullscreenClicked();

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    static QIcon loadIcon(const QString& baseName);

    TransportButton* prevFrameBtn_ = nullptr;
    TransportButton* playPauseBtn_ = nullptr;
    TransportButton* nextFrameBtn_ = nullptr;
    TransportButton* fullscreenBtn_ = nullptr;
    QSlider* slider_ = nullptr;
    QLabel* frameLabel_ = nullptr;

    QIcon playIcon_;
    QIcon pauseIcon_;
    QIcon fullscreenIcon_;
    QIcon exitFullscreenIcon_;
    bool playing_ = false;
};

} // namespace trace::ui
