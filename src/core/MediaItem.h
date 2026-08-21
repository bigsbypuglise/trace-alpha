#pragma once

#include <optional>
#include <string>
#include "core/SequenceDescriptor.h"

namespace trace::core {

// AudioFile: sound with no picture. frameSource_ stays null for it -- the
// transport runs on a synthetic frame index (duration x a nominal rate), and
// every frames-available consumer is expected to ask the kind rather than
// assume media-open implies frames-open.
enum class MediaKind { Unknown, VideoFile, StillImage, ImageSequence, AudioFile };

struct MediaItem {
    std::string path;
    MediaKind kind = MediaKind::Unknown;
    long long frameCount = -1;
    std::optional<SequenceDescriptor> sequence;
};

} // namespace trace::core
