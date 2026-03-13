#pragma once

#include <QWidget>
#include <QImage>
#include <QString>

namespace trace::ui {

class ViewerWidget final : public QWidget {
    Q_OBJECT
public:
    explicit ViewerWidget(QWidget* parent = nullptr);

    void setImage(const QImage& image);
    void clearImage();
    void setCenterText(const QString& text);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;
    bool hasImage_ = false;
    QString centerText_ = "Drop media or File > Open";
};

} // namespace trace::ui
