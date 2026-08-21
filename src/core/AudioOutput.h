#pragma once

#include <QString>
#include <memory>

namespace trace::core {

// What the HUD reports about the audio path. Sync is the number that matters:
// audio is the master clock, so a video frame that is not where the audio
// clock says it should be is the defect this readout exists to expose.
struct AudioPerfStats {
    bool available = false;      // file has an audio stream Trace could open
    bool disabledByEnv = false;  // TRACE_NO_AUDIO=1: control test, not a failure
    bool playing = false;
    bool muted = false;
    QString codecName;
    int sampleRate = 0;
    int channels = 0;
    double clockSeconds = 0.0;   // media time currently hitting the speakers
    double bufferedMs = 0.0;     // decoded audio waiting to be handed to the device
    long long underruns = 0;     // times the device asked for data we did not have
    bool ended = false;          // audio stream ran out before video did

    // Raw sink numbers behind the clock. Present because the clock pinning at
    // its start value froze the picture once already, and the derived figure
    // alone could not say which term was wrong.
    long long processedUSecs = 0;
    long long sinkBufferBytes = 0;
    long long sinkFreeBytes = 0;
    int sinkState = -1;

    // Buffer geometry. The startup churn this instrumentation was added for was
    // caused by the device buffer being twice what the clock constants assumed,
    // so requested-vs-actual has to be visible rather than inferred.
    long long sinkBufferRequestedBytes = 0;
    long long ringCapacityBytes = 0;
    // Durations, computed against the device's real bytes-per-frame. Not
    // derivable from the byte counts alone at the call site: this device hands
    // back float stereo (8 bytes/frame), so assuming 16-bit doubles every
    // figure -- which is exactly the misreading that made a 250ms buffer look
    // like 500ms.
    double deviceBufferMs = 0.0;
    double ringCapacityMs = 0.0;
    double startupFillMs = 0.0;   // wall time spent priming the ring before start
    long long silenceBytes = 0;   // padding handed to the device on short reads

    // Clock-control health. `clockUpdates` must advance exactly once per video
    // tick: telemetry that steps the loop changes playback, which is the bug
    // the advance/peek split exists to prevent.
    long long clockUpdates = 0;
    double smoothedLatencyMs = 0.0;
    double snapThresholdMs = 0.0;
    long long clockSnaps = 0;
};

// Decoded audio for the currently open file, played on the default output
// device. Owns its own demuxer and decode thread: sharing the video decoder's
// AVFormatContext would mean locking it against the seek-heavy video path on
// every packet, and the video path is deliberately single-threaded.
//
// Deliberately silent outside 1x forward playback. J-K-L off-speeds, reverse,
// scrubbing and stepping do not produce sound in this build.
class AudioOutput {
public:
    AudioOutput();
    ~AudioOutput();

    AudioOutput(const AudioOutput&) = delete;
    AudioOutput& operator=(const AudioOutput&) = delete;

    // Returns true when an audio stream was opened. A file with no audio is
    // not an error: false is returned with `error` empty.
    bool open(const QString& path, QString& error);
    void close();
    bool hasAudio() const;

    // Begins playback from `startSeconds` of media time. Seeks the audio
    // demuxer, refills, and starts the device.
    void start(double startSeconds);
    void stop();
    bool isPlaying() const;

    // Media time of the audio currently being heard, in seconds. This is the
    // master clock while playing; meaningless when stopped.
    //
    // Split in two deliberately. The clock is a first-order control loop, so
    // *reading* it used to step it: clockSeconds() was declared const, mutated
    // the loop state, and was called both by the playback tick and by the HUD's
    // stats() -- which doubled the effective gain and meant turning the HUD on
    // changed playback timing. advanceClock() is the single mutating step and
    // belongs to the video tick alone; every observer uses peekClock().
    double advanceClock();
    double peekClock() const;

    // False until the device has actually begun emitting sound. Between
    // sink->start() and the buffer filling, processedUSecs() is still below the
    // device-latency term, so the clock's raw reading is pinned at its start
    // value while wall time runs on. Driving video off a pinned clock stalls
    // the playhead for exactly one buffer duration and then lurches -- which is
    // where the startup hold/skip churn came from, and why the churn scaled
    // with buffer size. Video runs on the wall clock until this goes true.
    bool clockReady() const;

    // Number of times advanceClock() has run. Exposed so the tick can assert it
    // steps the loop exactly once per presented frame.
    long long clockUpdateCount() const;

    // True once the audio stream has been fully played out. Video keeps its own
    // clock past this point so a short audio track cannot truncate playback.
    bool ended() const;

    // The container's claim about the stream's length, in seconds; 0 when
    // nothing is open or the container states none. Read live off the open
    // demuxer -- open() is the only thing that changes it, so it is stable for
    // the life of a file. Added for audio-only media, whose synthetic frame
    // count is derived from it.
    double durationSeconds() const;
    // The FILE's sample rate and channel count, as distinct from the DEVICE's
    // that stats() reports -- stats().sampleRate is the output mix format, which
    // is the right thing for the sync diagnostics and the wrong thing for an
    // inspector describing the media.
    int sourceSampleRate() const;
    int sourceChannels() const;

    void setMuted(bool muted);
    bool isMuted() const;

    // Playback volume, as the UI's 0..1 fraction (the volume slider,
    // 2026-08-20). Mapped onto the sink's linear gain PERCEPTUALLY inside this
    // class -- QtAudio::convertVolume's logarithmic-to-linear -- so half slider
    // sounds like half volume rather than reading -6dB as barely quieter.
    // A GAIN ON THE SINK AND NOTHING ELSE: it never touches the audio clock,
    // the ring or the decode thread, which is the roadmap's own rule -- audio
    // owns rate and position during 1x playback, and that is what removed the
    // hold/skip churn. Composes with mute: the sink gets 0 while muted and this
    // gain otherwise, and unmuting restores it.
    void setVolume(double fraction);
    double volume() const;

    AudioPerfStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace trace::core
