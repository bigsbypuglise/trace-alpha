#pragma once

#include <algorithm>
#include <thread>
#include <vector>

namespace trace::core {

// SPLIT AN IMAGE INTO HORIZONTAL ROW BANDS AND RUN `fn(y0, rows)` ON EACH.
//
// Extracted from ColorTransform::apply so the colour stage and the float display
// mapping cannot drift apart on the one decision that matters here: bands are
// ROW RANGES, so each is a contiguous sub-image and no tile seam is possible for
// a per-pixel operation. Both callers are per-pixel.
//
// The calling thread takes the last band, including the remainder rows, rather
// than idling while N others work.
template <typename Fn>
void runInRowBands(int height, Fn&& fn) {
    if (height <= 0) return;

    const unsigned hw = std::thread::hardware_concurrency();
    int bands = static_cast<int>(hw == 0 ? 1u : hw);
    // Past 16 the per-frame thread cost stops paying for itself, and a small
    // picture is not worth splitting at all.
    bands = std::min(bands, 16);
    constexpr int kMinRowsPerBand = 64;
    bands = std::min(bands, height / kMinRowsPerBand);
    bands = std::max(bands, 1);

    if (bands == 1) {
        fn(0, height);
        return;
    }

    const int rowsPer = height / bands;
    std::vector<std::thread> workers;
    workers.reserve(static_cast<std::size_t>(bands) - 1);
    for (int b = 0; b < bands - 1; ++b)
        workers.emplace_back([&fn, b, rowsPer]() { fn(b * rowsPer, rowsPer); });
    fn((bands - 1) * rowsPer, height - (bands - 1) * rowsPer);
    for (auto& t : workers) t.join();
}

} // namespace trace::core
