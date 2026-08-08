#include "core/ScrubDecodeWorker.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>

#include <algorithm>
#include <utility>

namespace trace::core {

ScrubDecodeWorker::ScrubDecodeWorker() = default;

ScrubDecodeWorker::~ScrubDecodeWorker() {
    stop();
}

void ScrubDecodeWorker::start(VideoDecoderFFmpeg* decoder, QObject* notifyTarget,
                              std::function<void()> onResult) {
    stop();
    decoder_ = decoder;
    notifyTarget_ = notifyTarget;
    onResult_ = std::move(onResult);
    if (!decoder_) return;

    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
        hasResult_ = false;
        result_ = ScrubResult{};
        leaseRevoked_ = false;
        stop_ = false;
        inDecoder_ = false;
    }
    workerActive_.store(false);
    cancel_.store(false);
    activeGeneration_.store(-1);
    latestGeneration_.store(-1);
    requestsPosted_.store(0);
    requestsCoalesced_.store(0);
    resultsStale_.store(0);
    lastCancelWaitMs_ = 0.0;
    maxCancelWaitMs_ = 0.0;

    // Installed once, for the decoder's whole life. It is gated on
    // workerActive_, so while the UI thread is driving the decoder
    // synchronously the predicate is a false test and cannot abandon a walk
    // the UI is waiting on -- which it otherwise would, since the generation
    // moves whenever the user drags.
    decoder_->setCancelPredicate([this]() { return cancelRequested(); });

    thread_ = std::thread([this]() { run(); });
}

void ScrubDecodeWorker::stop() {
    if (!thread_.joinable()) {
        decoder_ = nullptr;
        return;
    }
    // Cancel first, then join. The reverted attempt joined a worker that could
    // be mid-GOP-walk with no cancellation check inside it, which blocked the
    // UI thread for the length of a decode.
    cancel_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        pending_.reset();
    }
    wake_.notify_all();
    thread_.join();

    if (decoder_) decoder_->setCancelPredicate(nullptr);
    decoder_ = nullptr;
    notifyTarget_ = nullptr;
    onResult_ = nullptr;
    cancel_.store(false);
    workerActive_.store(false);
    std::lock_guard<std::mutex> lock(mutex_);
    stop_ = false;
    hasResult_ = false;
    result_ = ScrubResult{};
}

void ScrubDecodeWorker::post(const ScrubRequest& request) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) return;
        // Depth 1. A target that arrives while another is still pending
        // replaces it; it never queues behind it, which is failure mode 2 from
        // the post-mortem.
        if (pending_.has_value()) ++requestsCoalesced_;
        pending_ = request;
        leaseRevoked_ = false;
    }
    ++requestsPosted_;
    latestGeneration_.store(request.generation, std::memory_order_release);
    cancel_.store(false, std::memory_order_release);
    wake_.notify_one();
}

void ScrubDecodeWorker::supersede(long long generation) {
    latestGeneration_.store(generation, std::memory_order_release);
}

bool ScrubDecodeWorker::busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return inDecoder_ || pending_.has_value();
}

bool ScrubDecodeWorker::takeResult(ScrubResult& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!hasResult_) return false;
    out = std::move(result_);
    result_ = ScrubResult{};
    hasResult_ = false;
    return true;
}

double ScrubDecodeWorker::revokeLease() {
    QElapsedTimer waitTimer;
    waitTimer.start();

    cancel_.store(true, std::memory_order_release);
    std::unique_lock<std::mutex> lock(mutex_);
    // Dropping the pending target as well as cancelling the active one: the
    // UI thread is taking the decoder back, so nothing queued for the worker
    // may run afterwards.
    pending_.reset();
    leaseRevoked_ = true;
    idle_.wait(lock, [this]() { return !inDecoder_; });
    lock.unlock();

    lastCancelWaitMs_ = static_cast<double>(waitTimer.nsecsElapsed()) / 1'000'000.0;
    maxCancelWaitMs_ = std::max(maxCancelWaitMs_, lastCancelWaitMs_);
    return lastCancelWaitMs_;
}

void ScrubDecodeWorker::run() {
    for (;;) {
        ScrubRequest request;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this]() {
                return stop_ || (pending_.has_value() && !leaseRevoked_);
            });
            if (stop_) return;
            request = *pending_;
            pending_.reset();
            // Taken under the lock together with the flag the UI thread waits
            // on, so there is no window where the UI believes the decoder is
            // free while this thread is about to enter it.
            inDecoder_ = true;
        }

        // --- the decoder is this thread's, exclusively, until inDecoder_ is
        // cleared below. The UI thread does not touch it in this window. ---
        activeGeneration_.store(request.generation, std::memory_order_release);
        workerActive_.store(true, std::memory_order_release);

        ScrubResult result;
        result.requestedFrame = request.frame;
        result.generation = request.generation;

        QElapsedTimer decodeTimer;
        decodeTimer.start();
        decoder_->setPlaybackDirection(request.direction);
        QString error;
        // The frame is decoded into a member so the conversion pool sees a
        // steady number of outstanding references across requests, exactly as
        // the UI thread's videoFrameBuffer_ does. Copying it into the result
        // is a refcount bump.
        const bool ok = decoder_->decodeFrameAt(
            request.frame, workerFrame_, error,
            VideoDecoderFFmpeg::RequestMode::Scrub);
        result.decodeMs = static_cast<double>(decodeTimer.nsecsElapsed()) / 1'000'000.0;

        result.ok = ok;
        result.abandoned = !ok && decoder_->lastRequestWasAbandoned();
        result.error = ok ? QString() : error;
        if (ok) result.frame = workerFrame_;
        result.perf = decoder_->perfStats();
        for (int i = 0; i < static_cast<int>(IoPhase::Count); ++i) {
            result.io[i] = decoder_->ioStats(static_cast<IoPhase>(i));
        }

        workerActive_.store(false, std::memory_order_release);
        // --- decoder released ---

        if (result.generation != latestGeneration_.load(std::memory_order_acquire)) {
            ++resultsStale_;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            inDecoder_ = false;
            result_ = std::move(result);
            hasResult_ = true;
        }
        idle_.notify_all();

        // Delivered through the target's event loop. The UI thread re-checks
        // the generation on receipt: this thread's check above can go stale in
        // the gap between the two, and the boundary that matters is the one
        // immediately before the frame reaches the viewer.
        if (notifyTarget_ && onResult_) {
            auto callback = onResult_;
            QMetaObject::invokeMethod(notifyTarget_, [callback]() { callback(); },
                                      Qt::QueuedConnection);
        }
    }
}

} // namespace trace::core
