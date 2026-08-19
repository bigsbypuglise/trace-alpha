#pragma once

#include <QString>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>

#include "core/MediaIoSource.h"
#include "core/VideoDecoderFFmpeg.h"
#include "core/VideoFrame.h"

class QObject;

namespace trace::core {

// Monotonic nanoseconds on one clock BOTH threads can read, so a timestamp
// taken on the UI thread and one taken on the worker are subtractable. Two
// QElapsedTimer instances are not: each measures from its own start(). On
// Windows both this and QElapsedTimer sit on QueryPerformanceCounter, so the
// cost is one QPC read.
//
// It exists for the round-trip split (--scrub-selftest): post-to-delivery for
// a scrub request, separated from the decode inside it. The leading hypothesis
// for the Threadripper MP4 scrub report is that the CROSS-THREAD cost is the
// machine-dependent term -- playback decodes synchronously on the UI thread and
// is reported fine there, which is exactly the split a per-request itinerary
// can see and no throughput figure can.
qint64 scrubMonotonicNs();

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
    // What the decoder should treat this as. Scrub by default, so the drag path
    // is unchanged by this field existing.
    //
    // The reverse shuttle posts Playback: a drag preview is deliberately
    // reduced-resolution above 1920px, and continuous reverse is playback --
    // the frame on screen is the frame, not a preview of one. It also puts
    // reverse fills on the Playback fill budget rather than the drag's, which
    // is the honest place for that decision to be made.
    VideoDecoderFFmpeg::RequestMode mode = VideoDecoderFFmpeg::RequestMode::Scrub;

    // How many CONSECUTIVE frames this request covers, starting at `frame` and
    // advancing by `direction`. 1 is the original behaviour and is what every
    // caller except the drag chain uses, so the reverse shuttle is untouched by
    // this field existing.
    //
    // It exists because the chain's cost is a cross-thread round trip per
    // frame, and on cheap frames that round trip is the whole cost: a 1280x720
    // H.264 frame decodes in 0.12ms and was being delivered every 3.56ms. The
    // synchronous walk never had this problem because one slice covered
    // `ceil(gap * kScrubEase)` frames inside a single call; that ease was
    // dropped from the async path on the reasoning that "yielding is now free",
    // which is true of the yield and false of the round trip.
    //
    // Frames are decoded, delivered and presented individually and in order --
    // this batches the ASKING, never the showing. It is not sampling and it
    // does not skip.
    long long batch = 1;
    // Wall-clock ceiling on one batch, checked after each frame. This is what
    // makes the batch safe on heavy media without a codec- or size-conditional
    // branch: at 23ms a frame, ProRes 4444 exceeds any sane budget on its first
    // frame and the batch collapses to 1 by itself. On the worker the budget
    // bounds DELIVERY LATENCY -- how long the picture waits for the first frame
    // of a batch -- rather than UI-thread occupancy, which is what the same
    // number bounds on the synchronous path.
    double batchBudgetMs = 0.0;
    // When post() accepted this request, on scrubMonotonicNs()'s clock.
    // Stamped by post() itself, not by the caller, so every request carries it
    // whichever of the four posting sites built the struct.
    qint64 postedNs = -1;
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

    // The request's cross-thread itinerary, on scrubMonotonicNs()'s clock. The
    // consumer stamps its own "now" when it drains the result; the differences
    // split one round trip into post->wake (condition-variable scheduling),
    // decode (the work), and publish->drain (QueuedConnection plus event-loop
    // latency). Those are the terms the machine can change while the file and
    // the code stay identical, which is why they are carried per result rather
    // than inferred from throughput.
    qint64 postedNs = -1;    // post() accepted the request (UI thread)
    qint64 dequeuedNs = -1;  // the worker picked it up
    qint64 publishedNs = -1; // the whole batch was pushed and notify sent
    // Decode time of the WHOLE batch this result belongs to. The batch is
    // published together, so the first result's post-to-drain time includes
    // every decode in it; overhead is round trip minus THIS, not minus the one
    // frame's decodeMs.
    double batchDecodeTotalMs = 0.0;
    // Position within the batch. 0 carries the request-level timing; frames
    // after it share the same stamps and would double-count the round trip.
    int batchIndex = 0;
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
//
// One REQUEST may cover several consecutive frames (ScrubRequest::batch), and
// the results of one batch are published together. That does not weaken any of
// the above: cancellation is re-tested between frames of a batch, so a revoked
// lease still comes back within one frame's decode rather than one batch's, and
// every result still carries the generation it was asked under.
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

    // Pops the oldest undelivered result. Callers already loop on this, so a
    // batch drains in the order it was decoded without any of them changing.
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
    // What the batching actually achieved, as distinct from what was asked
    // for. `lastBatchDecoded` is frames produced by the most recent request and
    // `maxBatchDecoded` the high-water mark: a request that asks for 4 and is
    // cut to 1 by the budget reports 1 here, which is how heavy media shows
    // that the budget collapsed the batch rather than that the rule never ran.
    long long lastBatchDecoded() const { return lastBatchDecoded_.load(); }
    long long maxBatchDecoded() const { return maxBatchDecoded_.load(); }
    // Batches ended by the wall-clock budget with frames still wanted. This is
    // the 8ms walk budget -- one of the three constants tuned on the dev box --
    // actually binding, as distinct from a batch that simply completed: the two
    // end the loop the same way and only this counter tells them apart.
    long long batchBudgetCuts() const { return batchBudgetCuts_.load(); }

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

    // Undelivered results, oldest first. A deque rather than the original
    // single slot because one batched request produces several frames; depth is
    // bounded by ScrubRequest::batch, which the UI thread caps.
    std::deque<ScrubResult> results_;
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
    std::atomic<long long> lastBatchDecoded_{0};
    std::atomic<long long> maxBatchDecoded_{0};
    std::atomic<long long> batchBudgetCuts_{0};
    double lastCancelWaitMs_ = 0.0;
    double maxCancelWaitMs_ = 0.0;
};

} // namespace trace::core
