#pragma once

#include <QString>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "core/MediaIoSource.h"
#include "core/VideoDecoderFFmpeg.h"
#include "core/VideoFrame.h"

class QObject;

namespace trace::core {

// One frame of the scrub shuttle, asked for by the UI thread.
//
// `generation` is a value of MainWindow::requestGeneration_ sampled when the
// request was posted -- not a counter of this worker's own. There is exactly
// one generation counter in Trace and this is a record of which of its values
// was last handed across the boundary.
struct ScrubRequest {
    long long frame = -1;
    long long generation = -1;
    int direction = 1;
};

struct ScrubResult {
    // What was asked for, never what the decoder happened to land on. The
    // first reverted async attempt keyed results by the landed index, so an
    // off-target landing was filed under a key nobody asked for.
    long long requestedFrame = -1;
    long long generation = -1;
    // What was actually delivered. Its own frameIndex is the identity; a
    // disagreement with requestedFrame is visible rather than papered over.
    VideoFrame frame;
    bool ok = false;
    // False return because the target moved, not because the frame could not
    // be produced. Not an error, and not something to report to the user.
    bool abandoned = false;
    QString error;
    double decodeMs = 0.0;
    // Taken on the worker, published under the same lock as the result. The
    // HUD reads these rather than the live decoder, because while the lease is
    // out the decoder's counters are being written by this thread.
    VideoPerfStats perf;
    IoPhaseStats io[static_cast<int>(IoPhase::Count)];
};

// Runs random-access scrub decode off the UI thread.
//
// The decoder is NOT owned here and NOT duplicated here -- the single reverted
// attempt that opened a second VideoDecoderFFmpeg on the same file is why this
// class holds a bare pointer that is only valid while the UI thread has granted
// the lease. There is one decoder and one owner of it at any instant; this
// worker is that owner only between post() and the result being published, and
// only while the UI thread is deliberately not touching it.
//
// Depth 1, latest wins. A newer target OVERWRITES the pending one rather than
// queueing behind it, and a walk already running is abandoned at the decoder's
// cancellation checkpoint instead of being decoded to completion first.
class ScrubDecodeWorker {
public:
    ScrubDecodeWorker();
    ~ScrubDecodeWorker();

    ScrubDecodeWorker(const ScrubDecodeWorker&) = delete;
    ScrubDecodeWorker& operator=(const ScrubDecodeWorker&) = delete;

    // `notifyTarget` must live on the thread that will consume results; the
    // callback is delivered to it through the event loop, so the UI thread
    // learns about a result the same way it learns about anything else.
    void start(VideoDecoderFFmpeg* decoder, QObject* notifyTarget,
               std::function<void()> onResult);
    void stop();

    // Hand the worker a target. Clears any revocation, so posting is how the
    // UI thread says "the lease is yours".
    void post(const ScrubRequest& request);

    // Everything in flight is now stale, without posting new work.
    void supersede(long long generation);
    long long latestGeneration() const { return latestGeneration_.load(); }

    // A request is active or pending. The UI uses it to avoid stacking work:
    // the chain re-posts on delivery instead.
    bool busy() const;

    bool takeResult(ScrubResult& out);

    // Raise cancellation, drop pending work, and block until the worker is
    // parked outside the decoder. This is the ONLY way the lease comes back,
    // and it is bounded by one cancellation checkpoint -- roughly one packet
    // decode -- rather than by a whole GOP walk. Returns how long it took, in
    // ms, which is the cancellation latency that matters.
    double revokeLease();

    // Cancellation-latency accounting across the session.
    double lastCancelWaitMs() const { return lastCancelWaitMs_; }
    double maxCancelWaitMs() const { return maxCancelWaitMs_; }
    long long requestsPosted() const { return requestsPosted_.load(); }
    long long requestsCoalesced() const { return requestsCoalesced_.load(); }
    long long resultsStale() const { return resultsStale_.load(); }

private:
    void run();
    // Consulted by the decoder once per packet. Lock-free on purpose: it is
    // called from inside the GOP walk and must cost nothing.
    bool cancelRequested() const {
        return workerActive_.load(std::memory_order_acquire)
            && (cancel_.load(std::memory_order_acquire)
                || activeGeneration_.load(std::memory_order_acquire)
                       != latestGeneration_.load(std::memory_order_acquire));
    }

    VideoDecoderFFmpeg* decoder_ = nullptr;
    QObject* notifyTarget_ = nullptr;
    std::function<void()> onResult_;

    std::thread thread_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;

    std::optional<ScrubRequest> pending_;
    bool inDecoder_ = false;
    // Set by revokeLease so a request posted a moment earlier cannot be picked
    // up after the UI thread has decided it owns the decoder again.
    bool leaseRevoked_ = false;
    bool stop_ = false;

    ScrubResult result_;
    bool hasResult_ = false;
    // Touched only by the worker thread, and only while it holds the lease.
    // Held across requests so the conversion pool sees a steady number of
    // outstanding references rather than one that collapses between frames --
    // the same reason MainWindow keeps videoFrameBuffer_.
    VideoFrame workerFrame_;

    std::atomic<bool> workerActive_{false};
    std::atomic<bool> cancel_{false};
    std::atomic<long long> activeGeneration_{-1};
    std::atomic<long long> latestGeneration_{-1};

    std::atomic<long long> requestsPosted_{0};
    std::atomic<long long> requestsCoalesced_{0};
    std::atomic<long long> resultsStale_{0};
    double lastCancelWaitMs_ = 0.0;
    double maxCancelWaitMs_ = 0.0;
};

} // namespace trace::core
