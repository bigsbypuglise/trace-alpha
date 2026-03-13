#pragma once

#include <optional>
#include <string>
#include "core/SequenceDescriptor.h"

namespace trace::core {

enum class MediaKind { Unknown, VideoFile, StillImage, ImageSequence };

struct MediaItem {
    std::string path;
    MediaKind kind = MediaKind::Unknown;
    long long frameCount = -1;
    std::optional<SequenceDescriptor> sequence;
};

} // namespace trace::core
