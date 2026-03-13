#pragma once

#include <QImage>
#include <QString>

namespace trace::core {

struct LoadedImageInfo {
    QString filePath;
    QString fileName;
    QString extension;
    int width = 0;
    int height = 0;
    int channels = 0;
    QImage image;
};

class StillImageLoader {
public:
    bool load(const QString& path, LoadedImageInfo& out, QString& error) const;

private:
    bool loadExr(const QString& path, LoadedImageInfo& out, QString& error) const;
};

} // namespace trace::core
