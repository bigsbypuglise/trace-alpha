#pragma once

#include <QWidget>
#include <QString>

namespace trace::ui {

class TransportOverlay final : public QWidget {
    Q_OBJECT
public:
    explicit TransportOverlay(QWidget* parent = nullptr);

    void setTransport(const QString& mode, qint64 frame, double speed, const QString& action);
    void setHudLine(const QString& line);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString mode_ = "Empty";
    qint64 frame_ = 0;
    double speed_ = 0.0;
    QString action_ = "Idle";
    QString hudLine_ = "No media loaded";
};

} // namespace trace::ui
