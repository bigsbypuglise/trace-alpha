#pragma once

#include <QWidget>
#include <QString>
#include <QStringList>

namespace trace::ui {

class TransportOverlay final : public QWidget {
    Q_OBJECT
public:
    explicit TransportOverlay(QWidget* parent = nullptr);

    void setTransport(const QString& mode, qint64 frame, double speed, const QString& action);
    // Accepts a '\n'-separated block; the overlay grows to fit so no field
    // gets clipped off the right edge at normal window widths.
    void setHudLine(const QString& line);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString mode_ = "Empty";
    qint64 frame_ = 0;
    double speed_ = 0.0;
    QString action_ = "Idle";
    QStringList hudLines_ = {"No media loaded"};
};

} // namespace trace::ui
