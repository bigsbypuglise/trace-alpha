#include "core/MediaIoSource.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QMutex>
#include <QMutexLocker>
#include <QThread>
#include <QWaitCondition>

#include <algorithm>

#ifdef Q_OS_WIN
// windows.h defines min/max as macros, which breaks std::min/std::max below.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#ifdef TRACE_WITH_FFMPEG
extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}
#endif

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace trace::core {

namespace {

// FFmpeg's own default AVIO buffer (IO_BUFFER_SIZE). Matching it exactly is
// the point: this layer must measure the existing read pattern, not create a
// new one. Changing it is a Phase 4/5 experiment, not part of instrumentation.
constexpr int kDefaultBufferSize = 32768;

bool envSet(const char* name, bool* value) {
    const QByteArray v = qgetenv(name);
    if (v.isEmpty()) return false;
    *value = (v != "0" && v.compare("false", Qt::CaseInsensitive) != 0);
    return true;
}

qint64 envInt64(const char* name, qint64 fallback) {
    const QByteArray v = qgetenv(name);
    if (v.isEmpty()) return fallback;
    bool ok = false;
    const qint64 parsed = v.toLongLong(&ok);
    return ok ? parsed : fallback;
}

// TRACE_IO_LOG=1 appends one line per closed file to %TEMP%\trace_iolog.txt,
// covering Playback and Seek (Open is uninteresting for this knob). Exists
// for the same reason TRACE_OPEN_LOG does: reading the read-ahead counters
// off a screenshot of the HUD would need OCR and would still miss whichever
// field scrolled off, where a file gives exact values a script can diff.
bool ioLogEnabled() {
    static const bool on = !qgetenv("TRACE_IO_LOG").isEmpty()
                        && qgetenv("TRACE_IO_LOG") != "0";
    return on;
}

void ioLogLine(const QString& line) {
    static QFile* f = [] {
        auto* file = new QFile(QDir::tempPath() + "/trace_iolog.txt");
        file->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
        return file;
    }();
    if (f && f->isOpen()) {
        f->write(line.toUtf8());
        f->write("\n");
        f->flush();
    }
}

} // namespace

struct MediaIoSource::Impl {
    QFile file;
    qint64 size = 0;
    qint64 pos = 0;
    qint64 lastReadEnd = -1;   // for sequential-vs-seek attribution
    IoPhase phase = IoPhase::Open;
    IoPhaseStats stats[static_cast<int>(IoPhase::Count)];
    StorageInfo storage;
    QElapsedTimer phaseClock;

    // --- asynchronous read path (remote sources only) ---
    //
    // The worker owns the QFile outright once started, so no file state is
    // ever touched from two threads. Each request is an absolute
    // seek-plus-read, which keeps the worker stateless between requests and
    // means a superseded request leaves nothing behind to unwind.
    struct IoRequest {
        qint64 offset = 0;
        int size = 0;
        uint8_t* dst = nullptr;   // FFmpeg's buffer; valid until we return
        qint64 got = 0;
        bool submitted = false;
        bool complete = false;
        unsigned generation = 0;
    };

    class IoWorker final : public QThread {
    public:
        explicit IoWorker(Impl* owner) : owner_(owner) {}
        void run() override { owner_->workerLoop(); }
    private:
        Impl* owner_;
    };

    QMutex ioMutex;
    QWaitCondition ioRequested;   // UI -> worker
    QWaitCondition ioComplete;    // worker -> UI
    IoRequest request;
    bool workerStop = false;
    std::unique_ptr<IoWorker> worker;
    StallPump stallPump;
    unsigned generation = 1;
    bool lastStale = false;

    // --- read-ahead (diagnostic, default off: TRACE_IO_READAHEAD=1) ---
    //
    // A fill-ahead buffer replacing the one-request-at-a-time worker above.
    // The worker reads sequentially ahead of wherever the demuxer has
    // consumed to, instead of waiting to be asked; a read is served only once
    // the FULL requested range is sitting in the buffer, never fragmented
    // (except at true EOF) -- that is the fix over the two prior attempts,
    // whose postmortem is in CLAUDE.md: v1 opened a second reader and
    // corrupted its own buffer; v2 served whatever partial amount was
    // available and fragmented every FFmpeg read into several, which dropped
    // sequentiality and drove up demuxer seeks. Every field below is set once
    // in open() before the worker starts and never mutated after, so reading
    // it from the worker without the lock is safe.
    //
    // These fields are set only when readAheadEnabled and the worker exists
    // (remote or TRACE_REMOTE_IO=1); the plain synchronous local path and the
    // legacy per-request async path never touch them.
    bool readAheadEnabled = false;
    int raCapacityBytes = 24 * 1024 * 1024;
    // Fill granularity. Matters a lot more than it looks: under a per-call
    // latency model (TRACE_IO_INJECT_DELAY_MS), a small chunk pays the fixed
    // round-trip cost once per chunk, so filling the whole buffer in many
    // small chunks can cost far more wall time than one big chunk would --
    // measured, a 256KB default under an 80ms/call delay took 7.7s just to
    // fill 24MB once. Default is a few MB, comfortably above the largest
    // ordinary FFmpeg read (~11.5MB on a 9K plate is the outlier; this is
    // sized for the common case and is a knob, not a constant, for exactly
    // that reason.
    int raChunkBytes = 4 * 1024 * 1024;

    // Mutable, protected by ioMutex.
    QByteArray raData;   // buffered bytes, contiguous from raBase
    qint64 raBase = 0;   // absolute file offset of raData[0]
    bool raEof = false;  // raBase + raData.size() is the end of the file
    unsigned raEpoch = 0; // bumped whenever raData is discarded and rebased

    // TRACE_IO_INJECT_KBPS=N: throttles the worker's own file reads to N
    // kbit/s, i.e. a bandwidth CAP -- the file itself cannot be made to
    // stream faster than this whatever the read pattern. Read-ahead cannot
    // beat this; it is here to reproduce a link that is genuinely too slow
    // for the file, not the case read-ahead targets.
    //
    // TRACE_IO_INJECT_DELAY_MS=N: a fixed per-read-call delay, independent
    // of bytes -- a round-trip/seek LATENCY, the thing a serialized
    // one-request-at-a-time reader cannot hide and a continuously-filling
    // worker can, because the round trip is paid once by the fill instead of
    // once per FFmpeg read. This is the knob CLAUDE.md's prior sessions
    // describe using ("an injected per-read delay to reproduce the cold
    // profile repeatably") and is the more honest model of what read-ahead
    // is actually for. Both are SYNTHETIC comparison tools, not a substitute
    // for measuring real remote storage. Set once in open().
    qint64 injectBytesPerSec = 0;
    qint64 injectDelayUs = 0;

    void throttleForBytes(qint64 bytes) const {
        if (injectDelayUs > 0) QThread::usleep(static_cast<unsigned long>(injectDelayUs));
        if (injectBytesPerSec <= 0 || bytes <= 0) return;
        const double seconds = static_cast<double>(bytes) / static_cast<double>(injectBytesPerSec);
        const qint64 us = static_cast<qint64>(seconds * 1'000'000.0);
        if (us > 0) QThread::usleep(static_cast<unsigned long>(us));
    }

    bool useAsyncReads() const { return worker != nullptr; }

    void workerLoop() {
        QMutexLocker lock(&ioMutex);
        while (!workerStop) {
            if (readAheadEnabled) {
                const bool full = raEof || raData.size() >= raCapacityBytes;
                if (full) {
                    ioRequested.wait(&ioMutex, 50);
                    continue;
                }
                const qint64 fillAt = raBase + raData.size();
                const qint64 want = std::min<qint64>(raChunkBytes, raCapacityBytes - raData.size());
                const unsigned myEpoch = raEpoch;
                lock.unlock();

                QByteArray chunk(want, Qt::Uninitialized);
                bool seekOk = true;
                if (file.pos() != fillAt) seekOk = file.seek(fillAt);
                qint64 got = seekOk ? file.read(chunk.data(), want) : -1;
                throttleForBytes(got);

                lock.relock();
                if (myEpoch != raEpoch) {
                    // Rebased onto a new position while this fill was in
                    // flight -- these bytes belong to an offset nobody wants.
                    continue;
                }
                if (got <= 0) {
                    raEof = true;
                } else {
                    raData.append(chunk.constData(), got);
                }
                ioComplete.wakeAll();
                continue;
            }

            if (!request.submitted) {
                ioRequested.wait(&ioMutex, 50);
                continue;
            }
            // Copy the parameters out, then do the blocking work unlocked so
            // the waiting thread can still observe state and cancel.
            const qint64 offset = request.offset;
            const int size = request.size;
            uint8_t* dst = request.dst;

            // Blocking work happens unlocked so the waiting thread can still
            // inspect state and mark the request superseded while it runs.
            ioMutex.unlock();
            qint64 got = -1;
            if (file.seek(offset)) {
                got = file.read(reinterpret_cast<char*>(dst), size);
            }
            throttleForBytes(got);
            ioMutex.lock();

            request.got = got;
            request.submitted = false;
            request.complete = true;
            ioComplete.wakeAll();
        }
    }

#ifdef TRACE_WITH_FFMPEG
    AVIOContext* avio = nullptr;
    unsigned char* buffer = nullptr;
#endif

    IoPhaseStats& current() { return stats[static_cast<int>(phase)]; }

    void chargePhaseTime() {
        if (phaseClock.isValid()) {
            current().activeMs += static_cast<double>(phaseClock.restart());
        } else {
            phaseClock.start();
        }
    }
};

MediaIoSource::MediaIoSource() : impl_(std::make_unique<Impl>()) {}

MediaIoSource::~MediaIoSource() { close(); }

int MediaIoSource::bufferSize() { return kDefaultBufferSize; }

namespace {

// Cheap key for the volume a path lives on, derived from the string alone.
// Deriving it this way matters: GetVolumePathNameW is itself one of the calls
// being cached, so the key must not require it. Generic - drive letters and
// UNC roots - with no LucidLink-specific knowledge.
QString volumeKeyFor(const QString& absolutePath) {
    const QString p = QDir::toNativeSeparators(absolutePath);
    if (p.startsWith(QLatin1String("\\\\"))) {
        // \\server\share -> keep both components.
        const int slash1 = p.indexOf(QLatin1Char('\\'), 2);
        if (slash1 < 0) return p;
        const int slash2 = p.indexOf(QLatin1Char('\\'), slash1 + 1);
        return (slash2 < 0 ? p : p.left(slash2)).toLower();
    }
    if (p.size() >= 2 && p.at(1) == QLatin1Char(':')) {
        return p.left(2).toLower();
    }
    return QStringLiteral("/");
}

struct ClassificationCache {
    QMutex mutex;
    QHash<QString, StorageInfo> entries;
    QElapsedTimer age;
};

ClassificationCache& classificationCache() {
    static ClassificationCache cache;
    return cache;
}

// Bounded staleness rather than device notifications: the only consequence of
// a stale entry is telemetry and tuning, never correctness, and a volume that
// disconnects will be re-judged within this window.
constexpr qint64 kClassificationTtlMs = 60'000;

} // namespace

void MediaIoSource::forgetStorageClassification() {
    ClassificationCache& cache = classificationCache();
    QMutexLocker lock(&cache.mutex);
    cache.entries.clear();
    cache.age.invalidate();
}

// Identifies the backing volume by querying it. Never writes a probe file --
// the LucidLink mount this exists for is live client storage.
StorageInfo MediaIoSource::classifyStorage(const QString& path) {
    StorageInfo info;
    const QString absolute = QFileInfo(path).absoluteFilePath();

    const QString key = volumeKeyFor(absolute);
    {
        ClassificationCache& cache = classificationCache();
        QMutexLocker lock(&cache.mutex);
        if (cache.age.isValid() && cache.age.elapsed() > kClassificationTtlMs) {
            cache.entries.clear();
            cache.age.invalidate();
        }
        const auto it = cache.entries.constFind(key);
        if (it != cache.entries.constEnd()) {
            StorageInfo cached = *it;
            cached.classifyCached = true;
            return cached;
        }
    }

#ifdef Q_OS_WIN
    const QString native = QDir::toNativeSeparators(absolute);
    wchar_t volumePath[MAX_PATH] = {};
    if (GetVolumePathNameW(reinterpret_cast<const wchar_t*>(native.utf16()),
                           volumePath, MAX_PATH)) {
        info.volumeRoot = QString::fromWCharArray(volumePath);

        info.driveType = GetDriveTypeW(volumePath);

        wchar_t label[MAX_PATH + 1] = {};
        wchar_t fsName[MAX_PATH + 1] = {};
        DWORD serial = 0, maxComp = 0, flags = 0;
        if (GetVolumeInformationW(volumePath, label, MAX_PATH, &serial, &maxComp,
                                  &flags, fsName, MAX_PATH)) {
            info.label = QString::fromWCharArray(label);
            info.filesystem = QString::fromWCharArray(fsName);
        }

        ULARGE_INTEGER freeAvail{}, total{}, freeTotal{};
        if (GetDiskFreeSpaceExW(volumePath, &freeAvail, &total, &freeTotal)) {
            info.capacityBytes = total.QuadPart;
            info.freeBytes = freeTotal.QuadPart;
        }
    }

    // DRIVE_REMOTE is definitive. LucidLink is not that: it presents as
    // DRIVE_FIXED with an NTFS-looking filesystem, so a virtual mount has to be
    // recognised by its implausible geometry instead -- it advertises a
    // petabyte-scale capacity with free == total, which no real fixed disk
    // does. Deliberately not keyed on the volume label ("Lucid"), which is
    // user-settable, nor on a drive letter.
    constexpr quint64 kImplausibleCapacity = 256ull * 1024 * 1024 * 1024 * 1024; // 256 TiB
    if (info.driveType == DRIVE_REMOTE) {
        info.remote = true;
        info.reason = QStringLiteral("DRIVE_REMOTE");
    } else if (info.capacityBytes >= kImplausibleCapacity
               && info.freeBytes == info.capacityBytes) {
        info.remote = true;
        info.reason = QStringLiteral("virtual mount (capacity %1 TiB, free == total)")
                          .arg(info.capacityBytes / (1024ull * 1024 * 1024 * 1024));
    } else {
        info.reason = QStringLiteral("fixed local volume");
    }
#else
    info.volumeRoot = QStringLiteral("/");
    info.reason = QStringLiteral("non-Windows: assumed local");
#endif

    // Developer override wins over any inference, in both directions, so the
    // remote path can be exercised against a local file and vice versa.
    bool forced = false;
    if (envSet("TRACE_REMOTE_IO", &forced)) {
        info.remote = forced;
        info.overridden = true;
        info.reason = forced ? QStringLiteral("forced by TRACE_REMOTE_IO")
                             : QStringLiteral("forced local by TRACE_REMOTE_IO=0");
    }

    {
        ClassificationCache& cache = classificationCache();
        QMutexLocker lock(&cache.mutex);
        if (!cache.age.isValid()) cache.age.start();
        cache.entries.insert(key, info);
    }
    return info;
}

#ifdef TRACE_WITH_FFMPEG

bool MediaIoSource::open(const QString& path, QString& error) {
    close();

    QElapsedTimer classifyTimer;
    classifyTimer.start();
    impl_->storage = classifyStorage(path);
    impl_->storage.classifyMs = static_cast<double>(classifyTimer.nsecsElapsed()) / 1'000'000.0;

    QElapsedTimer fileOpenTimer;
    fileOpenTimer.start();
    impl_->file.setFileName(path);
    // Unbuffered: Qt must not add a second layer of caching between FFmpeg's
    // AVIO buffer and the filesystem, or the measured read sizes would be
    // Qt's, not the ones the storage actually sees.
    if (!impl_->file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        error = QStringLiteral("Unable to open media file: %1").arg(impl_->file.errorString());
        return false;
    }
    // size() is included deliberately: on a virtual mount, stat can cost as
    // much as the open.
    impl_->size = impl_->file.size();
    impl_->storage.fileOpenMs = static_cast<double>(fileOpenTimer.nsecsElapsed()) / 1'000'000.0;
    impl_->pos = 0;
    impl_->lastReadEnd = -1;

    impl_->buffer = static_cast<unsigned char*>(av_malloc(kDefaultBufferSize));
    if (!impl_->buffer) {
        error = QStringLiteral("Unable to allocate media I/O buffer");
        impl_->file.close();
        return false;
    }

    impl_->avio = avio_alloc_context(
        impl_->buffer, kDefaultBufferSize,
        0,                    // read-only: this layer never writes
        impl_.get(),
        &MediaIoSource::readPacket,
        nullptr,
        &MediaIoSource::seekPacket);
    if (!impl_->avio) {
        av_free(impl_->buffer);
        impl_->buffer = nullptr;
        impl_->file.close();
        error = QStringLiteral("Unable to allocate AVIO context");
        return false;
    }

    // Diagnostic knobs, read once here: TRACE_IO_READAHEAD enables the
    // fill-ahead worker in place of the one-request-at-a-time path, and
    // TRACE_IO_INJECT_KBPS throttles the worker's own reads so a local file
    // can stand in for a slow remote link. Both are inert unless a worker
    // actually exists (remote storage, or TRACE_REMOTE_IO=1 forcing it).
    {
        bool ra = false;
        envSet("TRACE_IO_READAHEAD", &ra);
        impl_->readAheadEnabled = ra;
    }
    impl_->injectBytesPerSec = 0;
    const qint64 injectKbps = envInt64("TRACE_IO_INJECT_KBPS", 0);
    if (injectKbps > 0) impl_->injectBytesPerSec = (injectKbps * 1000) / 8;
    impl_->injectDelayUs = std::max<qint64>(0, envInt64("TRACE_IO_INJECT_DELAY_MS", 0)) * 1000;
    // Default comfortably exceeds the largest single read observed in this
    // project (~11.5MB, one packet on a 9K ProRes 4444 plate) -- a request
    // bigger than the capacity is still served correctly (see readPacket),
    // just fragmented, so this is headroom for the common case rather than a
    // hard requirement.
    const qint64 raMb = envInt64("TRACE_IO_READAHEAD_MB", 24);
    impl_->raCapacityBytes = static_cast<int>(std::max<qint64>(1, raMb) * 1024 * 1024);
    const qint64 raChunkKb = envInt64("TRACE_IO_READAHEAD_CHUNK_KB", 4096);
    impl_->raChunkBytes = static_cast<int>(std::min<qint64>(
        std::max<qint64>(64, raChunkKb) * 1024, impl_->raCapacityBytes));
    impl_->raData.clear();
    impl_->raBase = 0;
    impl_->raEof = false;
    impl_->raEpoch = 0;

    // Only remote sources get the worker. A local read completes in tens of
    // microseconds; routing it through a thread handoff would add cost to the
    // one path that was never at fault, and Phase 5 asks explicitly that the
    // local path not regress.
    if (impl_->storage.remote) {
        impl_->workerStop = false;
        impl_->worker = std::make_unique<Impl::IoWorker>(impl_.get());
        impl_->worker->start();
    }

    impl_->phase = IoPhase::Open;
    impl_->phaseClock.start();
    return true;
}

void MediaIoSource::setStallPump(StallPump pump) {
    if (!impl_) return;
    QMutexLocker lock(&impl_->ioMutex);
    impl_->stallPump = std::move(pump);
}

void MediaIoSource::cancelOutstanding() {
    if (!impl_) return;
    QMutexLocker lock(&impl_->ioMutex);
    ++impl_->generation;
}

bool MediaIoSource::lastReadWasStale() const {
    return impl_ && impl_->lastStale;
}

void MediaIoSource::close() {
    if (!impl_) return;

    // Stop the worker before anything it touches goes away. An in-flight read
    // is allowed to finish -- it is writing into a buffer we do not own -- so
    // teardown is bounded by one read rather than abandoning it mid-write.
    if (impl_->worker) {
        {
            QMutexLocker lock(&impl_->ioMutex);
            impl_->workerStop = true;
            ++impl_->generation;
            impl_->ioRequested.wakeAll();
        }
        impl_->worker->wait();
        impl_->worker.reset();
    }
    {
        QMutexLocker lock(&impl_->ioMutex);
        impl_->request = Impl::IoRequest{};
        impl_->lastStale = false;
        impl_->raData.clear();
        impl_->raBase = 0;
        impl_->raEof = false;
        ++impl_->raEpoch;
        // stallPump deliberately survives: it is the owner's policy for how to
        // wait, not per-file state, and open() calls close() first -- clearing
        // it here would silently disarm the pump for every file after the
        // first.
    }

    if (ioLogEnabled() && (impl_->stats[static_cast<int>(IoPhase::Playback)].reads > 0
                        || impl_->stats[static_cast<int>(IoPhase::Seek)].reads > 0)) {
        // Skip closes with nothing read at all -- a spare or probe-only
        // decoder that never touched a byte, of which there are several per
        // launch. Without this a script tailing the last line can land on
        // one of those instead of the session that actually played.
        const auto& p = impl_->stats[static_cast<int>(IoPhase::Playback)];
        const auto& sk = impl_->stats[static_cast<int>(IoPhase::Seek)];
        ioLogLine(QString(
            "readahead=%1 injectKbps=%2 injectDelayMs=%3 capMB=%4 | "
            "play rd=%5 MB=%6 seq=%7%% hits=%8 rebases=%9 seeks=%10 stall=%11 avgLat=%12ms activeMs=%13 | "
            "seek rd=%14 MB=%15 seq=%16%% hits=%17 rebases=%18 seeks=%19 stall=%20 avgLat=%21ms")
            .arg(impl_->readAheadEnabled ? "1" : "0")
            .arg(impl_->injectBytesPerSec > 0 ? (impl_->injectBytesPerSec * 8) / 1000 : 0)
            .arg(impl_->injectDelayUs / 1000)
            .arg(impl_->raCapacityBytes / (1024 * 1024))
            .arg(p.reads).arg(QString::number(p.bytes / (1024.0 * 1024.0), 'f', 1))
            .arg(QString::number(p.sequentialFraction() * 100.0, 'f', 1))
            .arg(p.bufferHits).arg(p.raRebases).arg(p.seeks).arg(p.stalls)
            .arg(QString::number(p.avgLatencyMs(), 'f', 3))
            .arg(QString::number(p.activeMs, 'f', 0))
            .arg(sk.reads).arg(QString::number(sk.bytes / (1024.0 * 1024.0), 'f', 1))
            .arg(QString::number(sk.sequentialFraction() * 100.0, 'f', 1))
            .arg(sk.bufferHits).arg(sk.raRebases).arg(sk.seeks).arg(sk.stalls)
            .arg(QString::number(sk.avgLatencyMs(), 'f', 3)));
    }

    if (impl_->avio) {
        // avio_context_free frees the context but not the buffer it was given,
        // and the context may have swapped that pointer, so take it back first.
        if (impl_->avio->buffer) {
            av_free(impl_->avio->buffer);
            impl_->buffer = nullptr;
        }
        avio_context_free(&impl_->avio);
        impl_->avio = nullptr;
    }
    if (impl_->buffer) {
        av_free(impl_->buffer);
        impl_->buffer = nullptr;
    }
    if (impl_->file.isOpen()) impl_->file.close();
    impl_->size = 0;
    impl_->pos = 0;
    impl_->lastReadEnd = -1;
    for (auto& s : impl_->stats) s = IoPhaseStats{};
    impl_->storage = StorageInfo{};
    impl_->phase = IoPhase::Open;
    impl_->phaseClock.invalidate();
}

AVIOContext* MediaIoSource::avio() const { return impl_ ? impl_->avio : nullptr; }

int MediaIoSource::readPacket(void* opaque, uint8_t* buf, int size) {
    auto* impl = static_cast<MediaIoSource::Impl*>(opaque);
    if (!impl || !impl->file.isOpen()) return AVERROR_EOF;

    IoPhaseStats& s = impl->current();

    QElapsedTimer t;
    t.start();

    qint64 got = -1;
    bool stale = false;
    double unservicedMaxMs = 0.0;

    if (!impl->useAsyncReads()) {
        // Local volumes keep the plain synchronous read: no worker, no
        // handoff, no added latency on a path that was never the problem.
        got = impl->file.read(reinterpret_cast<char*>(buf), size);
    } else if (impl->readAheadEnabled) {
        // Remote, read-ahead. Serve from the fill-ahead buffer -- the full
        // request or nothing, never fragmented, except at true EOF.
        QMutexLocker lock(&impl->ioMutex);
        const unsigned myGeneration = impl->generation;
        const qint64 want = size;
        // The worker stops filling once raData reaches raCapacityBytes, so a
        // single FFmpeg request bigger than that could otherwise wait for an
        // amount the buffer will never hold -- a hang, not a slowdown. Cap
        // what this read waits for at the buffer's ceiling; only a request
        // that itself exceeds the configured capacity is served fragmented.
        const qint64 target = std::min<qint64>(want, impl->raCapacityBytes);

        // Outside what is buffered (or about to be) is a real seek from
        // read-ahead's point of view: the buffer is stale for this position,
        // so discard it and refill from here rather than trying to patch a
        // gap. This also covers the very first read of a file (raData empty,
        // raBase 0): if pos is already 0 it is a no-op; if not, it rebases
        // exactly as any other seek would.
        if (impl->pos < impl->raBase || impl->pos > impl->raBase + impl->raData.size()) {
            impl->raData.clear();
            impl->raBase = impl->pos;
            impl->raEof = false;
            ++impl->raEpoch;
            ++s.raRebases;
            impl->ioRequested.wakeAll();
        }

        const qint64 haveAtStart = impl->raBase + impl->raData.size() - impl->pos;
        const bool hitImmediately = haveAtStart >= target;

        qint64 lastServicedNs = t.nsecsElapsed();
        for (;;) {
            const qint64 avail = impl->raBase + impl->raData.size() - impl->pos;
            if (avail >= target) break;
            if (impl->raEof) break; // whatever is left is all there will ever be
            if (impl->stallPump) {
                const double waited = static_cast<double>(t.elapsed());
                lock.unlock();
                impl->stallPump(waited);
                lock.relock();
                const qint64 nowNs = t.nsecsElapsed();
                unservicedMaxMs = std::max(
                    unservicedMaxMs,
                    static_cast<double>(nowNs - lastServicedNs) / 1'000'000.0);
                lastServicedNs = nowNs;
            } else {
                impl->ioComplete.wait(&impl->ioMutex, 20);
            }
        }

        const qint64 avail = impl->raBase + impl->raData.size() - impl->pos;
        got = std::min<qint64>(want, avail);
        if (got > 0) {
            const qint64 offsetInBuffer = impl->pos - impl->raBase;
            std::memcpy(buf, impl->raData.constData() + offsetInBuffer, static_cast<size_t>(got));
        }
        if (hitImmediately) ++s.bufferHits;
        stale = (myGeneration != impl->generation);

        // Compact once the consumed prefix is worth the memmove -- normal
        // case, amortizes the memmove over a chunk's worth of reads. `pos`
        // (about to advance by `got`) is the oldest byte anything could
        // still want, so nothing before it is ever dropped early.
        //
        // The second condition is load-bearing, not an optimisation: if the
        // buffer is sitting at capacity, the worker's fill loop is paused
        // (raData.size() >= raCapacityBytes) until something frees room. A
        // single fragmented read (want > capacity, see `target` above) can
        // consume the ENTIRE buffer in one shot, and when raChunkBytes ==
        // raCapacityBytes that lands exactly on the first condition's
        // boundary and never fires -- the worker stays paused forever
        // waiting for room only compaction can create, and playback hangs.
        // Measured: a 1MB TRACE_IO_READAHEAD_MB against ~2.4MB ProRes 4444
        // packets reproduces it every time without this.
        const qint64 consumedAfter = (impl->pos + got) - impl->raBase;
        const bool bufferAtCapacity = impl->raData.size() >= impl->raCapacityBytes;
        if (consumedAfter > 0 && (consumedAfter >= impl->raChunkBytes || bufferAtCapacity)) {
            impl->raData.remove(0, consumedAfter);
            impl->raBase += consumedAfter;
            // The worker's fill loop only re-checks "is there room" on its
            // own 50ms poll otherwise; waking it here is the difference
            // between resuming a stalled fill immediately and up to 50ms
            // later, on every compaction, not just the capacity-hang case.
            if (bufferAtCapacity) impl->ioRequested.wakeAll();
        }
    } else {
        // Remote, legacy one-request-at-a-time path. Hand the blocking
        // syscall to the worker and stay awake.
        QMutexLocker lock(&impl->ioMutex);
        impl->request = Impl::IoRequest{};
        impl->request.offset = impl->pos;
        impl->request.size = size;
        impl->request.dst = buf;
        impl->request.submitted = true;
        impl->request.generation = impl->generation;
        const unsigned myGeneration = impl->generation;
        impl->ioRequested.wakeAll();

        // The wait always runs to completion even when superseded: `buf`
        // belongs to FFmpeg and the worker is writing into it, so returning
        // early would hand the decoder a buffer still being filled. What
        // cancellation buys is that the caller learns the result is stale, not
        // that the read is abandoned.
        // The honest responsiveness metric is not how long the read took, but
        // the longest stretch in which the caller did nothing -- that is the
        // window in which the UI could not repaint or accept input.
        qint64 lastServicedNs = t.nsecsElapsed();
        while (!impl->request.complete) {
            if (impl->stallPump) {
                const double waited = static_cast<double>(t.elapsed());
                lock.unlock();
                impl->stallPump(waited);   // caller keeps its event loop alive
                lock.relock();
                const qint64 nowNs = t.nsecsElapsed();
                unservicedMaxMs = std::max(
                    unservicedMaxMs,
                    static_cast<double>(nowNs - lastServicedNs) / 1'000'000.0);
                lastServicedNs = nowNs;
            } else {
                impl->ioComplete.wait(&impl->ioMutex, 20);
            }
        }
        got = impl->request.got;
        stale = (myGeneration != impl->generation);
        impl->request = Impl::IoRequest{};
    }

    const double ms = static_cast<double>(t.nsecsElapsed()) / 1'000'000.0;
    impl->lastStale = stale;
    // Without a pump the caller was inside the syscall for the whole read.
    if (!impl->useAsyncReads() || !impl->stallPump) unservicedMaxMs = ms;
    s.callerBlockMaxMs = std::max(s.callerBlockMaxMs, unservicedMaxMs);
    if (ms >= MediaIoSource::kStallMs) {
        ++s.bufferingEvents;
        s.bufferingMsTotal += ms;
    }

    if (got < 0) return AVERROR(EIO);
    if (got == 0) return AVERROR_EOF;

    // Attribute before advancing: a read starting exactly where the last one
    // ended is the sequential pattern we want playback to consist of.
    if (impl->lastReadEnd >= 0 && impl->pos == impl->lastReadEnd) {
        s.sequentialBytes += got;
    }

    ++s.reads;
    s.bytes += got;
    s.minReadBytes = s.reads == 1 ? got : std::min(s.minReadBytes, got);
    s.maxReadBytes = std::max(s.maxReadBytes, got);
    s.latencyTotalMs += ms;
    s.latencyMaxMs = std::max(s.latencyMaxMs, ms);
    if (ms >= MediaIoSource::kStallMs) {
        ++s.stalls;
        s.stallMsTotal += ms;
    }

    impl->pos += got;
    impl->lastReadEnd = impl->pos;
    return static_cast<int>(got);
}

int64_t MediaIoSource::seekPacket(void* opaque, int64_t offset, int whence) {
    auto* impl = static_cast<MediaIoSource::Impl*>(opaque);
    if (!impl || !impl->file.isOpen()) return AVERROR(EIO);

    // FFmpeg asks for the size through the seek callback rather than a
    // separate entry point.
    if (whence == AVSEEK_SIZE) return impl->size;

    qint64 target = offset;
    if (whence == SEEK_CUR) target = impl->pos + offset;
    else if (whence == SEEK_END) target = impl->size + offset;
    if (target < 0) return AVERROR(EINVAL);

    if (target != impl->pos) {
        IoPhaseStats& s = impl->current();
        ++s.seeks;
        s.seekDistanceTotal += std::llabs(target - impl->pos);
        // A seek breaks the sequential run.
        impl->lastReadEnd = -1;
    }

    // On the async path the worker seeks as part of each read, so the file
    // handle is never touched from this thread; `pos` is the only state that
    // needs updating. Reads are absolute, so nothing can be left half-seeked.
    if (!impl->useAsyncReads() && !impl->file.seek(target)) return AVERROR(EIO);
    impl->pos = target;
    return target;
}

#else // !TRACE_WITH_FFMPEG

bool MediaIoSource::open(const QString&, QString& error) {
    error = QStringLiteral("Built without FFmpeg");
    return false;
}
void MediaIoSource::close() {}
AVIOContext* MediaIoSource::avio() const { return nullptr; }

#endif // TRACE_WITH_FFMPEG

bool MediaIoSource::isOpen() const { return impl_ && impl_->file.isOpen(); }

void MediaIoSource::setPhase(IoPhase phase) {
    if (!impl_ || impl_->phase == phase) return;
    impl_->chargePhaseTime();
    impl_->phase = phase;
}

IoPhase MediaIoSource::phase() const { return impl_ ? impl_->phase : IoPhase::Open; }

IoPhaseStats MediaIoSource::stats(IoPhase phase) const {
    if (!impl_) return {};
    IoPhaseStats s = impl_->stats[static_cast<int>(phase)];
    // Fold in the time the current phase has accrued but not yet been charged,
    // so a live readout is not a phase behind.
    if (phase == impl_->phase && impl_->phaseClock.isValid()) {
        s.activeMs += static_cast<double>(impl_->phaseClock.elapsed());
    }
    return s;
}

const StorageInfo& MediaIoSource::storage() const {
    static const StorageInfo kEmpty;
    return impl_ ? impl_->storage : kEmpty;
}

qint64 MediaIoSource::fileSize() const { return impl_ ? impl_->size : 0; }
qint64 MediaIoSource::position() const { return impl_ ? impl_->pos : 0; }

} // namespace trace::core
