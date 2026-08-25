#pragma once

// PER-STAGE PROFILING FOR THE IMAGE-SEQUENCE PLAYBACK PATH.
//
// WHY THIS EXISTS. The 27-channel DWAA sequence presents at ~29% of real time
// with a ~190ms handler against a 41.67ms budget, while `exrprobe --read`
// measures its selected channel span at ~35ms standalone. That ~5x gap has been
// recorded as unexplained since the cadence instrument was built, and the
// 2026-08-24 pass-cycling leg narrowed it rather than closing it: passes whose
// standalone reads differ by ~10ms present identically, so the missing ~150ms
// is INVARIANT to which channels are read.
//
// A handler figure cannot say where its own time went, and this project has
// twice reached for an architecture change on a stage that turned out not to be
// the dominant one. So the stages are timed individually first.
//
// COST WHEN OFF: one function-local static bool read and a predictable branch
// per scope. No clock is sampled, no allocation is made, no file is touched.
// `TRACE_SEQ_PROFILE=1` turns it on; it is off in every shipping path.
//
// THE STAGES ARE NOT ALL PER-FRAME, AND THAT IS THE POINT. A single playback
// tick on this path can perform up to THREE loads -- the frame being presented
// plus prefetchNeighbors()' two synchronous neighbour loads -- so every loader
// stage carries its own CALL COUNT beside its total. Dividing a loader total by
// the frame count without that count is how a three-load tick reads as one
// expensive load.

#include <cstdint>

namespace trace::core::seqprofile {

enum class Stage : int {
    // --- inside loadExr, once per LOAD (not once per presented frame) ---
    Open = 0,      // ImageInput::open + spec: repeated per-frame open/metadata
    Group,         // channel names, groupExrChannels, duplicate lookup, choosePass
    Alloc,         // FrameBuffer::allocate -- w*h*16 bytes, fresh, per load
    AlphaFill,     // the opaque-alpha pre-pass over the whole float buffer
    Read,          // read_image: the actual EXR decode
    Tail,          // compression attribute, QFileInfo, close, struct fill

    // --- inside the playback tick, once per presented frame ---
    CacheHit,      // FrameCache::get served the frame being presented
    CacheMiss,     // it did not, and the loader ran
    Prefetch,      // prefetchNeighbors() in full, both neighbours
    PrefetchDecline, // stride-aware: prediction judged unreliable, nothing loaded
    Map,           // mapFloatToDisplay / ColorTransform: mapping + pixel conversion
    Upload,        // renderer_->setFrame: the GPU upload
    Present,       // the repaint the tick asks for

    Count
};

bool enabled();

// Add an elapsed time to a stage and count one call against it.
void add(Stage s, double ms);

// Count a call with no time (used for load counts and cache hits/misses).
void bump(Stage s);

// One presented frame has been dealt with. Advances the frame denominator.
void frameBoundary();

// Write the accumulated table and reset. Safe to call when disabled (no-op).
void dump(const char* tag);

// RAII scope timer. Samples the clock only when the profiler is enabled.
class Scope {
public:
    explicit Scope(Stage s);
    ~Scope();
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Stage stage_;
    bool on_;
    std::int64_t startNs_;
};

} // namespace trace::core::seqprofile

// Explicit `Scope guard{Stage::X};` at each site rather than a macro: a macro
// pasting __LINE__ needs two-level expansion to work at all, and a named guard
// is what the rest of this codebase uses for the same job (recordHandler).
