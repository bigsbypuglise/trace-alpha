#pragma once

#include <QString>

namespace trace::core::TimeFormat {

double frameToSeconds(long long frame, double fps);
QString formatSeconds(double seconds, int decimals = 3);

// Elapsed time in HH:MM:SS:FF, COUNTED FROM ZERO AT THE FIRST FRAME.
//
// Renamed from frameToTimecode at spec phase 7, and the rename is the point.
// This is an elapsed-time conversion; it is not the source's timecode and never
// was. The spec forbids labelling one as the other -- *"do not label an
// elapsed-time conversion as source timecode"* -- and the HUD had been doing
// exactly that, printing this value under `Timecode:` for every file including
// the ones that carry a real start timecode and the ones that carry none.
//
// It is kept rather than deleted because elapsed time in frames-and-seconds is
// genuinely useful and is what a file without source timecode can honestly show.
QString frameToElapsed(long long frame, double fps);

// A source timecode, as SMPTE defines it.
//
// `dropFrame` is a property of the TIMECODE, not of the rate: 29.97 material can
// legitimately carry either, and the container says which. It is only meaningful
// at 30000/1001 and 60000/1001; anywhere else it is ignored rather than
// rejected, because a file that claims it at 24fps is malformed in a way that
// should not stop it playing.
struct Timecode {
    int hours = 0;
    int minutes = 0;
    int seconds = 0;
    int frames = 0;
    bool dropFrame = false;
};

// Parses "HH:MM:SS:FF" and "HH:MM:SS;FF". The separator before the frames field
// is what carries drop-frame -- a semicolon (or a period, which some tools
// write) means drop-frame. Returns false on anything it cannot read, and NEVER
// guesses: an unparseable timecode is the same as no timecode, because a wrong
// one is worse than none in a review tool.
bool parseTimecode(const QString& text, Timecode& out);

QString formatTimecode(const Timecode& tc);

// The nominal SMPTE rate for a source rate: 24 for 23.976, 30 for 29.97, 60 for
// 59.94. Timecode counts whole frames per second, so 30000/1001 material runs
// 30 timecode frames per timecode second and lets the *clock* drift instead --
// which is the entire reason drop-frame exists.
int nominalRate(int fpsNum, int fpsDen);

// Whether drop-frame arithmetic applies at this rate at all. True only for
// 30000/1001 and 60000/1001 families.
bool dropFrameApplies(int fpsNum, int fpsDen);

// Timecode <-> frame count, both directions, against the exact rational.
//
// The pair is exact and round-trips. Drop-frame skips frame numbers 0 and 1 (or
// 0..3 at 60) at the start of every minute except every tenth, so the count is
// not a simple product and the two conversions have to agree about that or a Go
// to Timecode would land somewhere near the right place instead of on it.
long long timecodeToFrames(const Timecode& tc, int fpsNum, int fpsDen);
Timecode framesToTimecode(long long frames, int fpsNum, int fpsDen, bool dropFrame);

} // namespace trace::core::TimeFormat
