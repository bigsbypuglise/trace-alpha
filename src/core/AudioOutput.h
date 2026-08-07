#pragma once

#include <QString>
#include <memory>

namespace trace::core {

// What the HUD reports about the audio path. Sync is the number that matters:
// audio is the master clock, so a video frame that is not where the audio
// clock says it should be is the defect this readout exists to expose.
struct AudioPerfStats {
    bool available = false;      // file has an audio stream Trace could open
    bool playing = false;
    bool muted = false;
    QString codecName;
    int sampleRate = 0;
    int channels = 0;
    double clockSeconds = 0.0;   // media time currently hitting the speakers
    double bufferedMs = 0.0;     // decoded audio waiting to be handed to the device
    long long underruns = 0;     // times the device asked for data we did not have
    bool ended = false;          // audio stream ran out before video did
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
    double clockSeconds() const;

    // True once the audio stream has been fully played out. Video keeps its own
    // clock past this point so a short audio track cannot truncate playback.
    bool ended() const;

    void setMuted(bool muted);
    bool isMuted() const;

    AudioPerfStats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace trace::core
