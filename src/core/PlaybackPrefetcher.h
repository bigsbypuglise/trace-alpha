#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

namespace trace::core {

// Background decode queue for playback.
// Responsibilities:
//  - Decode/load upcoming frames off the UI thread.
//  - Keep a small ready-map keyed by frame index.
// Presentation is intentionally kept in MainWindow/UI thread.
class PlaybackPrefetcher {
public:
    struct PrefetchedFrame {
        long long frameIndex = -1;
        QString path;
        QImage image;
        int channels = 0;
    };

    PlaybackPrefetcher();
    ~PlaybackPrefetcher();

    void stop();

    void configureForSequence(QStringList framePaths);
    bool configureForVideo(const QString& videoPath, QString& error);

    void request(long long frameIndex);
    std::optional<PrefetchedFrame> take(long long frameIndex);

    int queuedCount() const;

private:
    enum class Mode {
        None,
        Sequence,
        Video,
    };

    void resetUnlocked();
    void startWorkerIfNeeded();
    void workerLoop();

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    bool stopRequested_ = false;

    Mode mode_ = Mode::None;
    QStringList framePaths_;
    QString videoPath_;

    std::deque<long long> pending_;
    std::unordered_map<long long, PrefetchedFrame> ready_;
    static constexpr size_t kMaxReadyFrames = 12;
};

} // namespace trace::core
