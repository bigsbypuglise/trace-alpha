#include "core/MediaIoSource.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>

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

// Identifies the backing volume by querying it. Never writes a probe file --
// the LucidLink mount this exists for is live client storage.
StorageInfo MediaIoSource::classifyStorage(const QString& path) {
    StorageInfo info;
    const QString absolute = QFileInfo(path).absoluteFilePath();

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

    return info;
}

#ifdef TRACE_WITH_FFMPEG

bool MediaIoSource::open(const QString& path, QString& error) {
    close();

    impl_->storage = classifyStorage(path);

    impl_->file.setFileName(path);
    // Unbuffered: Qt must not add a second layer of caching between FFmpeg's
    // AVIO buffer and the filesystem, or the measured read sizes would be
    // Qt's, not the ones the storage actually sees.
    if (!impl_->file.open(QIODevice::ReadOnly | QIODevice::Unbuffered)) {
        error = QStringLiteral("Unable to open media file: %1").arg(impl_->file.errorString());
        return false;
    }
    impl_->size = impl_->file.size();
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

    impl_->phase = IoPhase::Open;
    impl_->phaseClock.start();
    return true;
}

void MediaIoSource::close() {
    if (!impl_) return;
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
    const qint64 got = impl->file.read(reinterpret_cast<char*>(buf), size);
    const double ms = static_cast<double>(t.nsecsElapsed()) / 1'000'000.0;

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

    if (!impl->file.seek(target)) return AVERROR(EIO);
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
