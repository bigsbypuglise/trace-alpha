#include "core/PlaybackPrefetcher.h"

#include <QFileInfo>

#include "core/StillImageLoader.h"
#include "core/VideoDecoderFFmpeg.h"

namespace trace::core {

PlaybackPrefetcher::PlaybackPrefetcher() {
    startWorkerIfNeeded();
}

PlaybackPrefetcher::~PlaybackPrefetcher() {
    stop();
}

void PlaybackPrefetcher::startWorkerIfNeeded() {
    if (worker_.joinable()) return;
    stopRequested_ = false;
    worker_ = std::thread([this]() { workerLoop(); });
}

void PlaybackPrefetcher::resetUnlocked() {
    pending_.clear();
    ready_.clear();
}

void PlaybackPrefetcher::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopRequested_ = true;
        resetUnlocked();
        mode_ = Mode::None;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void PlaybackPrefetcher::configureForSequence(QStringList framePaths) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = Mode::Sequence;
        framePaths_ = std::move(framePaths);
        videoPath_.clear();
        resetUnlocked();
        stopRequested_ = false;
    }
    startWorkerIfNeeded();
    cv_.notify_all();
}

bool PlaybackPrefetcher::configureForVideo(const QString& videoPath, QString& error) {
    // Validate open here so callers can fail fast on unsupported codecs.
    VideoDecoderFFmpeg probe;
    if (!probe.open(videoPath, error)) return false;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        mode_ = Mode::Video;
        videoPath_ = videoPath;
        framePaths_.clear();
        resetUnlocked();
        stopRequested_ = false;
    }
    startWorkerIfNeeded();
    cv_.notify_all();
    return true;
}

void PlaybackPrefetcher::request(long long frameIndex) {
    if (frameIndex < 0) return;

    std::lock_guard<std::mutex> lock(mutex_);
    if (mode_ == Mode::None) return;
    if (ready_.find(frameIndex) != ready_.end()) return;
    for (long long pendingFrame : pending_) {
        if (pendingFrame == frameIndex) return;
    }

    pending_.push_back(frameIndex);
    cv_.notify_one();
}

std::optional<PlaybackPrefetcher::PrefetchedFrame> PlaybackPrefetcher::take(long long frameIndex) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = ready_.find(frameIndex);
    if (it == ready_.end()) return std::nullopt;
    auto out = it->second;
    ready_.erase(it);
    return out;
}

int PlaybackPrefetcher::queuedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<int>(pending_.size() + ready_.size());
}

void PlaybackPrefetcher::workerLoop() {
    StillImageLoader stillLoader;
    VideoDecoderFFmpeg videoDecoder;
    QString openedVideoPath;

    for (;;) {
        long long frameIndex = -1;
        Mode mode = Mode::None;
        QString framePath;
        QString videoPath;

        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this]() { return stopRequested_ || !pending_.empty(); });
            if (stopRequested_) return;

            frameIndex = pending_.front();
            pending_.pop_front();
            mode = mode_;

            if (mode == Mode::Sequence) {
                if (frameIndex >= 0 && frameIndex < static_cast<long long>(framePaths_.size())) {
                    framePath = framePaths_[static_cast<int>(frameIndex)];
                }
            } else if (mode == Mode::Video) {
                videoPath = videoPath_;
            }
        }

        PrefetchedFrame prefetched;
        prefetched.frameIndex = frameIndex;

        if (mode == Mode::Sequence && !framePath.isEmpty()) {
            LoadedImageInfo info;
            QString error;
            if (stillLoader.load(framePath, info, error)) {
                prefetched.path = info.filePath;
                prefetched.image = info.image;
                prefetched.channels = info.channels;
            }
        } else if (mode == Mode::Video && !videoPath.isEmpty()) {
            QString error;
            if (openedVideoPath != videoPath) {
                videoDecoder.close();
                if (videoDecoder.open(videoPath, error)) openedVideoPath = videoPath;
            }
            if (!openedVideoPath.isEmpty()) {
                QImage image;
                if (videoDecoder.decodeFrameAt(frameIndex, image, error)) {
                    prefetched.frameIndex = videoDecoder.currentFrame();
                    prefetched.path = videoPath;
                    prefetched.image = image;
                    prefetched.channels = 3;
                }
            }
        }

        if (prefetched.image.isNull()) continue;

        std::lock_guard<std::mutex> lock(mutex_);
        ready_[prefetched.frameIndex] = std::move(prefetched);
        if (ready_.size() > kMaxReadyFrames) {
            // Drop oldest inserted map entries by frame index scan biasing away from latest requests.
            auto oldest = ready_.begin();
            for (auto it = ready_.begin(); it != ready_.end(); ++it) {
                if (it->first < oldest->first) oldest = it;
            }
            ready_.erase(oldest);
        }
    }
}

} // namespace trace::core
