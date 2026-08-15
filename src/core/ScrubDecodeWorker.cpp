#include "core/ScrubDecodeWorker.h"

#include <QElapsedTimer>
#include <QMetaObject>
#include <QObject>

#include <algorithm>
#include <deque>
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
        results_.clear();
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
    lastBatchDecoded_.store(0);
    maxBatchDecoded_.store(0);
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
    results_.clear();
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
    // UNDELIVERED RESULTS COUNT AS BUSY, and with a batch that is load-bearing
    // rather than tidiness. The caller derives its next target from the frame
    // last PRESENTED, so between the worker publishing a batch and the UI
    // thread draining it there is a window where the decoder has advanced N
    // frames and activeScrubFrame_ has not. A request posted inside that window
    // asks for a frame the decoder has already passed, which is a BACKWARD
    // move: a seek plus a GOP walk, in the middle of a forward drag.
    //
    // At batch 1 the window was one frame wide and re-asking for it was a cache
    // hit, so nothing showed. At batch 4 it cost `seeks 1 -> 13`,
    // `ra-walk 0.00 -> 44.85` and `ins 371 -> 986` on the 720p file, and ate
    // most of the gain the batch was supposed to deliver.
    return inDecoder_ || pending_.has_value() || !results_.empty();
}

bool ScrubDecodeWorker::takeResult(ScrubResult& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (results_.empty()) return false;
    out = std::move(results_.front());
    results_.pop_front();
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
    // AND DROPPING RESULTS ALREADY PRODUCED, which busy() now reports. Every
    // caller of reclaimDecoder() bumps the generation before revoking, so
    // anything sitting here is stale by construction and the UI thread would
    // discard it at the delivery boundary anyway -- but it would still be
    // COUNTED, and a worker that reads busy() after handing the decoder back is
    // a worker nothing can be posted to.
    //
    // That froze the next shuttle run outright. endShuttleRun() reclaims the
    // lease and startShuttleRun() then calls pumpShuttleQueue(), which returns
    // early on busy(): the new run never posted its first request, the queue
    // stayed empty, and the tick starved forever. The run-is-over test at the
    // top of the tick reads busy() too, so it could not even end. Measured as
    // one random "expected moving" case per full transitions matrix -- 2 of 50
    // against 0 of 50 on a control built from efda50c.
    results_.clear();
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
        decoder_->setPlaybackDirection(request.direction);

        // One request, up to `batch` CONSECUTIVE frames. Every frame is decoded
        // and published individually and in order; what the batch removes is
        // the round trip between them, which on cheap frames was 30x the cost
        // of the frame itself.
        const long long wanted = std::max<long long>(1, request.batch);
        std::deque<ScrubResult> produced;
        QElapsedTimer batchTimer;
        batchTimer.start();

        for (long long i = 0; i < wanted; ++i) {
            // Re-tested BETWEEN frames, which is what keeps every guarantee the
            // single-frame version made. revokeLease() raises cancel_ and then
            // waits on inDecoder_, so without this the lease would come back in
            // one BATCH rather than in one frame; with it the worst case is
            // unchanged. It also means a reversal stops a batch that is now
            // heading the wrong way instead of decoding it out.
            if (i > 0 && cancelRequested()) break;

            ScrubResult result;
            result.requestedFrame = request.frame + request.direction * i;
            result.generation = request.generation;

            QElapsedTimer decodeTimer;
            decodeTimer.start();
            QString error;
            // The frame is decoded into a member so the conversion pool sees a
            // steady number of outstanding references across requests, exactly
            // as the UI thread's videoFrameBuffer_ does. Copying it into the
            // result is a refcount bump.
            const bool ok = decoder_->decodeFrameAt(
                result.requestedFrame, workerFrame_, error, request.mode);
            result.decodeMs =
                static_cast<double>(decodeTimer.nsecsElapsed()) / 1'000'000.0;

            result.ok = ok;
            result.abandoned = !ok && decoder_->lastRequestWasAbandoned();
            result.error = ok ? QString() : error;
            if (ok) result.frame = workerFrame_;
            result.perf = decoder_->perfStats();
            for (int j = 0; j < static_cast<int>(IoPhase::Count); ++j) {
                result.io[j] = decoder_->ioStats(static_cast<IoPhase>(j));
            }

            const bool keepGoing = ok;
            produced.push_back(std::move(result));
            // A failure or an abandonment ends the batch. Continuing would ask
            // for frames past one that could not be produced, and the caller
            // re-posts from what was actually presented anyway.
            if (!keepGoing) break;
            // Budget checked AFTER the frame, so a batch always delivers at
            // least one -- the same shape as the synchronous walk, where
            // `steps >= 1` and the budget is tested at the end of the body.
            if (request.batchBudgetMs > 0.0
                && static_cast<double>(batchTimer.nsecsElapsed()) / 1'000'000.0
                       >= request.batchBudgetMs) {
                break;
            }
        }

        workerActive_.store(false, std::memory_order_release);
        // --- decoder released ---

        const long long decoded = static_cast<long long>(produced.size());
        lastBatchDecoded_.store(decoded);
        if (decoded > maxBatchDecoded_.load()) maxBatchDecoded_.store(decoded);

        const long long latest = latestGeneration_.load(std::memory_order_acquire);
        for (const auto& r : produced) {
            if (r.generation != latest) ++resultsStale_;
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            inDecoder_ = false;
            for (auto& r : produced) results_.push_back(std::move(r));
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
