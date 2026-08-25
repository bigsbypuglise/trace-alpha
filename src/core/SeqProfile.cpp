#include "core/SeqProfile.h"

#include <QByteArray>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QTextStream>
#include <QtGlobal>

#include <array>

namespace trace::core::seqprofile {
namespace {

struct Bucket {
    double totalMs = 0.0;
    long long calls = 0;
    double maxMs = 0.0;
};

// A monotonic clock shared by every scope, started once. QElapsedTimer's
// nsecsElapsed() is the same clock the playback scheduler is built on, so a
// stage total and a handler figure are the same kind of measurement.
QElapsedTimer& clock() {
    static QElapsedTimer t = [] {
        QElapsedTimer e;
        e.start();
        return e;
    }();
    return t;
}

std::array<Bucket, static_cast<std::size_t>(Stage::Count)>& buckets() {
    static std::array<Bucket, static_cast<std::size_t>(Stage::Count)> b{};
    return b;
}

long long& frames() {
    static long long n = 0;
    return n;
}

const char* stageName(Stage s) {
    switch (s) {
        case Stage::Open:        return "open+spec";
        case Stage::Group:       return "group/choose";
        case Stage::Alloc:       return "buffer alloc";
        case Stage::AlphaFill:   return "alpha prefill";
        case Stage::Read:        return "read_image";
        case Stage::Tail:        return "loader tail";
        case Stage::CacheLookup: return "cache lookup";
        case Stage::Prefetch:    return "prefetch (all)";
        case Stage::Map:         return "map+convert";
        case Stage::Upload:      return "upload";
        case Stage::Present:     return "present";
        case Stage::Count:       break;
    }
    return "?";
}

QString logPath() {
    return QDir::temp().filePath(QStringLiteral("trace_seqprofile.txt"));
}

} // namespace

bool enabled() {
    static const bool on = [] {
        const QByteArray v = qgetenv("TRACE_SEQ_PROFILE");
        const bool e = !v.isEmpty() && v != "0";
        if (e) {
            // Written at static init, for tickLogEnabled()'s recorded reason:
            // a run that produced no samples must be distinguishable from a run
            // where the knob was never set, and a lazily created file cannot
            // tell those apart.
            QFile f(logPath());
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                QTextStream out(&f);
                out << "# trace sequence-path stage profile\n"
                    << "# TRACE_SEQ_PROFILE=1. Loader stages run ONCE PER LOAD, and one\n"
                    << "# playback tick can perform up to three loads (the presented frame\n"
                    << "# plus prefetchNeighbors()' two). Read `calls` beside every total;\n"
                    << "# per-frame is total/frames, per-call is total/calls, and on the\n"
                    << "# loader rows those are different questions.\n";
            }
            clock().start();
        }
        return e;
    }();
    return on;
}

void add(Stage s, double ms) {
    if (!enabled()) return;
    Bucket& b = buckets()[static_cast<std::size_t>(s)];
    b.totalMs += ms;
    b.calls += 1;
    if (ms > b.maxMs) b.maxMs = ms;
}

void bump(Stage s) {
    if (!enabled()) return;
    buckets()[static_cast<std::size_t>(s)].calls += 1;
}

void frameBoundary() {
    if (!enabled()) return;
    frames() += 1;
}

void dump(const char* tag) {
    if (!enabled()) return;

    QFile f(logPath());
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) return;
    QTextStream out(&f);

    const long long n = frames();
    out << "\n=== " << (tag ? tag : "") << "  frames=" << n << " ===\n";
    out << QStringLiteral("%1 %2 %3 %4 %5\n")
               .arg(QStringLiteral("stage"), -16)
               .arg(QStringLiteral("total ms"), 10)
               .arg(QStringLiteral("calls"), 7)
               .arg(QStringLiteral("per call"), 9)
               .arg(QStringLiteral("per frame"), 10);

    double accounted = 0.0;
    for (int i = 0; i < static_cast<int>(Stage::Count); ++i) {
        const Bucket& b = buckets()[static_cast<std::size_t>(i)];
        if (b.calls == 0) continue;
        const auto s = static_cast<Stage>(i);
        // Prefetch is the SUM of its own inner loader stages, so counting it in
        // the total would double-count every neighbour load. It is reported as
        // a cross-check on the loader rows, not as a term beside them.
        if (s != Stage::Prefetch) accounted += b.totalMs;
        out << QStringLiteral("%1 %2 %3 %4 %5\n")
                   .arg(QString::fromLatin1(stageName(s)), -16)
                   .arg(b.totalMs, 10, 'f', 1)
                   .arg(b.calls, 7)
                   .arg(b.calls ? b.totalMs / static_cast<double>(b.calls) : 0.0, 9, 'f', 2)
                   .arg(n ? b.totalMs / static_cast<double>(n) : 0.0, 10, 'f', 2);
    }
    out << QStringLiteral("%1 %2 %3 %4\n")
               .arg(QStringLiteral("ACCOUNTED"), -16)
               .arg(accounted, 10, 'f', 1)
               .arg(QString(), 7)
               .arg(n ? accounted / static_cast<double>(n) : 0.0, 20, 'f', 2);

    for (auto& b : buckets()) b = Bucket{};
    frames() = 0;
}

Scope::Scope(Stage s) : stage_(s), on_(enabled()), startNs_(0) {
    if (on_) startNs_ = clock().nsecsElapsed();
}

Scope::~Scope() {
    if (!on_) return;
    const std::int64_t endNs = clock().nsecsElapsed();
    add(stage_, static_cast<double>(endNs - startNs_) / 1e6);
}

} // namespace trace::core::seqprofile
