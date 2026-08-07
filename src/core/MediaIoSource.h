#pragma once

#include <QString>
#include <cstdint>
#include <memory>

struct AVIOContext;

namespace trace::core {

// Which activity a read belongs to. Forward playback and random access have
// completely different I/O signatures, and averaging them together hides the
// only question that matters: is *ordinary playback* read-starved?
enum class IoPhase {
    Open = 0,    // probe, header parse, stream info
    Playback,    // sequential forward decode
    Seek,        // scrub drag and step landings
    Count
};

struct IoPhaseStats {
    long long reads = 0;
    long long bytes = 0;
    long long minReadBytes = 0;
    long long maxReadBytes = 0;
    long long seeks = 0;              // seek callbacks that actually moved
    long long seekDistanceTotal = 0;  // absolute, bytes
    long long sequentialBytes = 0;    // read at the previous read's end offset
    double latencyTotalMs = 0.0;
    double latencyMaxMs = 0.0;
    long long stalls = 0;             // single reads over kStallMs
    double stallMsTotal = 0.0;
    double activeMs = 0.0;            // wall time this phase has been current

    double avgReadBytes() const {
        return reads > 0 ? static_cast<double>(bytes) / static_cast<double>(reads) : 0.0;
    }
    double avgLatencyMs() const {
        return reads > 0 ? latencyTotalMs / static_cast<double>(reads) : 0.0;
    }
    double sequentialFraction() const {
        return bytes > 0 ? static_cast<double>(sequentialBytes) / static_cast<double>(bytes) : 0.0;
    }
    // Throughput measured against time spent reading, not wall clock: this is
    // what the storage delivered when asked, independent of how often we asked.
    double readMbps() const {
        return latencyTotalMs > 0.0
             ? (static_cast<double>(bytes) * 8.0 / 1'000'000.0) / (latencyTotalMs / 1000.0)
             : 0.0;
    }
};

// How the media's storage was classified, and the raw evidence behind it, so a
// wrong call can be diagnosed from the HUD instead of guessed at.
struct StorageInfo {
    QString volumeRoot;
    QString filesystem;
    QString label;
    unsigned int driveType = 0;       // Windows DRIVE_* value
    quint64 capacityBytes = 0;
    quint64 freeBytes = 0;
    bool remote = false;
    QString reason;                   // why `remote` was decided
    bool overridden = false;          // TRACE_REMOTE_IO forced the answer

    // Cost of finding all of the above, and of the open() syscall itself.
    // Both are separate from any read, and on a virtual mount either can
    // dominate the time to open a file -- including, potentially, the
    // classification we perform ourselves.
    double classifyMs = 0.0;
    double fileOpenMs = 0.0;
    bool classifyCached = false;      // served from the per-volume cache
};

// A read-only, instrumented file source handed to FFmpeg as a custom
// AVIOContext. Deliberately pass-through to begin with: the buffer size
// matches FFmpeg's own default so the measurements describe the read pattern
// Trace *already* has, rather than one this layer introduced.
class MediaIoSource {
public:
    MediaIoSource();
    ~MediaIoSource();

    MediaIoSource(const MediaIoSource&) = delete;
    MediaIoSource& operator=(const MediaIoSource&) = delete;

    bool open(const QString& path, QString& error);
    void close();
    bool isOpen() const;

    // Non-owning; valid until close(). Assign to AVFormatContext::pb before
    // avformat_open_input and set AVFMT_FLAG_CUSTOM_IO.
    AVIOContext* avio() const;

    void setPhase(IoPhase phase);
    IoPhase phase() const;

    IoPhaseStats stats(IoPhase phase) const;
    const StorageInfo& storage() const;
    qint64 fileSize() const;
    qint64 position() const;

    // Size of the buffer FFmpeg reads through. Reported so the HUD can show
    // what the read granularity actually is.
    static int bufferSize();

    // A read slower than this is recorded as a stall.
    static constexpr double kStallMs = 20.0;

    // Classifies without opening anything. Never writes to the volume.
    // Results are cached per volume root: the Win32 volume queries cost ~8.5ms
    // on a virtual mount versus ~0.9ms locally, and the answer is identical for
    // every file on the same volume.
    static StorageInfo classifyStorage(const QString& path);

    // Drops the classification cache. Exists so a volume that is disconnected
    // and reconnected is not judged by a stale entry.
    static void forgetStorageClassification();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;

    // AVIOContext callbacks. Members rather than free functions so they can
    // reach Impl, which stays private.
    static int readPacket(void* opaque, uint8_t* buf, int size);
    static int64_t seekPacket(void* opaque, int64_t offset, int whence);
};

} // namespace trace::core
