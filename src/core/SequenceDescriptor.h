#pragma once

#include <string>
#include <vector>

namespace trace::core {

struct SequenceDescriptor {
    std::string directory;
    std::string prefix;
    std::string suffix;
    int padWidth = 0;
    int firstFrame = 0;
    int lastFrame = 0;
    std::vector<int> frames;
    std::string pattern;
};

} // namespace trace::core
