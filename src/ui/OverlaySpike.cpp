#include "ui/OverlaySpike.h"

#include <QByteArray>
#include <QDebug>
#include <QEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPushButton>
#include <QResizeEvent>
#include <QWidget>

namespace trace::ui {
namespace {

// Reports hover, clicks, wheel and double-click on whatever it watches, so the
// spike can prove input actually lands rather than inferring it from the fact
// that a widget is visible.
class InputProbe final : public QObject {
public:
    InputProbe(QObject* parent, QString name) : QObject(parent), name_(std::move(name)) {}

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        switch (event->type()) {
            case QEvent::Enter:
                qWarning().noquote() << "OVERLAY-SPIKE hover-enter" << name_;
                break;
            case QEvent::MouseButtonPress:
                qWarning().noquote() << "OVERLAY-SPIKE press" << name_;
                break;
            case QEvent::MouseButtonDblClick:
                qWarning().noquote() << "OVERLAY-SPIKE double-click" << name_;
                break;
            case QEvent::Wheel:
                qWarning().noquote() << "OVERLAY-SPIKE wheel" << name_;
                break;
            default:
                break;
        }
        return QObject::eventFilter(watched, event);
    }

private:
    QString name_;
};

// Keeps the spike widgets positioned over the video rect as it resizes. A
// layout would do this too, but the point is to mimic a floating transport,
// which is placed rather than laid out.
class SpikeLayoutWatcher final : public QObject {
public:
    SpikeLayoutWatcher(QWidget* viewer, QWidget* label, QWidget* button, QWidget* panel)
        : QObject(viewer), viewer_(viewer), label_(label), button_(button), panel_(panel) {
        reposition();
    }

    void reposition() {
        const int w = viewer_->width();
        const int h = viewer_->height();
        label_->setGeometry((w - 260) / 2, (h - 40) / 2, 260, 40);
        panel_->setGeometry((w - 320) / 2, h - 90, 320, 60);
        button_->setGeometry((w - 120) / 2, h - 78, 120, 36);
    }

protected:
    bool eventFilter(QObject* watched, QEvent* event) override {
        if (watched == viewer_ && event->type() == QEvent::Resize) reposition();
        return QObject::eventFilter(watched, event);
    }

private:
    QWidget* viewer_;
    QWidget* label_;
    QWidget* button_;
    QWidget* panel_;
};

int g_variant = 0;

} // namespace

int overlaySpikeVariant() { return g_variant; }

void installOverlaySpike(QWidget* viewer, int variant) {
    if (!viewer || variant <= 0) return;
    g_variant = variant;

    const bool native = (variant >= 2);

    auto* panel = new QWidget(viewer);
    panel->setObjectName("spikePanel");
    // Translucent, to test whether anything of the video shows through a Qt
    // widget stacked over a native surface.
    panel->setStyleSheet("background-color: rgba(20, 20, 20, 160);"
                         " border: 1px solid rgba(255,255,255,60); border-radius: 6px;");

    auto* label = new QLabel("OVERLAY SPIKE - centered label", viewer);
    label->setAlignment(Qt::AlignCenter);
    label->setStyleSheet("color: #FFFFFF; background-color: rgba(200, 30, 30, 200);"
                         " font-size: 14px; border-radius: 4px;");

    auto* button = new QPushButton("Spike Play/Pause", viewer);
    button->setStyleSheet("color: #000000; background-color: #E0E0E0;"
                          " font-size: 13px; border-radius: 4px;");
    // Focus is part of what is being tested: a native sibling window can take
    // focus away from the main window in a way a plain child never does.
    button->setFocusPolicy(Qt::StrongFocus);

    QObject::connect(button, &QPushButton::clicked, [] {
        qWarning().noquote() << "OVERLAY-SPIKE button-clicked";
    });

    // Variant 3 asks the follow-up question variant 2 raises: a native sibling
    // is visible and takes input, but its alpha is flattened, because sibling
    // child windows do not per-pixel composite with each other. A translucent
    // floating transport is the stated design, so whether this can be recovered
    // decides whether the native-sibling route is actually usable.
    const bool translucent = (variant >= 3);

    for (QWidget* w : {panel, static_cast<QWidget*>(label), static_cast<QWidget*>(button)}) {
        if (native) {
            // Each becomes its own HWND, a sibling of the D3D11 surface. Z-order
            // is then the window manager's business, and raise() has to be
            // enough to put it above the video.
            w->setAttribute(Qt::WA_NativeWindow, true);
        }
        if (translucent) {
            w->setAttribute(Qt::WA_TranslucentBackground, true);
            w->setAttribute(Qt::WA_NoSystemBackground, true);
        }
        w->setMouseTracking(true);
        w->show();
        w->raise();
    }

    // Stacking order among the spike widgets themselves: panel behind, controls
    // in front. Independent of whether any of them beat the video surface.
    panel->stackUnder(button);
    label->raise();
    button->raise();

    auto* labelProbe = new InputProbe(viewer, QStringLiteral("label"));
    label->installEventFilter(labelProbe);
    auto* buttonProbe = new InputProbe(viewer, QStringLiteral("button"));
    button->installEventFilter(buttonProbe);
    auto* panelProbe = new InputProbe(viewer, QStringLiteral("panel"));
    panel->installEventFilter(panelProbe);
    // The viewer itself, so "did the video window swallow the event" is
    // answerable rather than inferred from silence on the overlay.
    auto* viewerProbe = new InputProbe(viewer, QStringLiteral("viewer"));
    viewer->installEventFilter(viewerProbe);

    auto* watcher = new SpikeLayoutWatcher(viewer, label, button, panel);
    viewer->installEventFilter(watcher);

    qWarning().noquote() << "OVERLAY-SPIKE installed variant" << variant
                         << (native ? "(native sibling HWNDs)" : "(plain Qt children)");
}

} // namespace trace::ui
