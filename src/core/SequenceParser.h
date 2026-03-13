#pragma once

#include <optional>
#include <string>
#include "core/SequenceDescriptor.h"

namespace trace::core {

class SequenceParser {
public:
    static std::optional<SequenceDescriptor> detect(const std::string& filePath);
    static std::optional<int> extractFrameNumber(const std::string& filePath);
};

} // namespace trace::core
