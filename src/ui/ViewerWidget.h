#pragma once

#include <QWidget>
#include <QImage>
#include <QString>

namespace trace::ui {

struct ViewerPerfStats {
    double lastPaintMs = 0.0;
    double avgPaintMs = 0.0;
    long long samples = 0;
};

class ViewerWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void clearImage();
    void setCenterText(const QString& text);
    const ViewerPerfStats& perfStats() const { return perfStats_; }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
    bool hasImage_ = false;
    QString centerText_ = "Drop media or File > Open";
    ViewerPerfStats perfStats_{};
};

} // namespace trace::ui
