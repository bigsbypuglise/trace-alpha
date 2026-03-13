#include "core/SequenceParser.h"

#include <algorithm>
#include <filesystem>
#include <regex>

namespace trace::core {

namespace {
const std::regex kPattern(R"(^(.*?)(\d+)(\.[^.]+)$)");
}

std::optional<SequenceDescriptor> SequenceParser::detect(const std::string& filePath) {
    namespace fs = std::filesystem;
    fs::path p(filePath);
    if (!fs::exists(p) || !fs::is_regular_file(p)) return std::nullopt;

    std::smatch m;
    const std::string name = p.filename().string();
    if (!std::regex_match(name, m, kPattern)) return std::nullopt;

    SequenceDescriptor seq;
    seq.directory = p.parent_path().string();
    seq.prefix = m[1].str();
    seq.suffix = m[3].str();
    seq.padWidth = static_cast<int>(m[2].str().size());

    for (const auto& entry : fs::directory_iterator(p.parent_path())) {
        if (!entry.is_regular_file()) continue;
        const std::string cand = entry.path().filename().string();
        std::smatch cm;
        if (!std::regex_match(cand, cm, kPattern)) continue;
        if (cm[1].str() != seq.prefix || cm[3].str() != seq.suffix) continue;
        if (static_cast<int>(cm[2].str().size()) != seq.padWidth) continue;
        seq.frames.push_back(std::stoi(cm[2].str()));
    }

    if (seq.frames.size() < 3) return std::nullopt;

    std::sort(seq.frames.begin(), seq.frames.end());
    seq.firstFrame = seq.frames.front();
    seq.lastFrame = seq.frames.back();
    seq.pattern = seq.prefix + "%0" + std::to_string(seq.padWidth) + "d" + seq.suffix;
    return seq;
}

std::optional<int> SequenceParser::extractFrameNumber(const std::string& filePath) {
    namespace fs = std::filesystem;
    std::smatch m;
    const std::string name = fs::path(filePath).filename().string();
    if (!std::regex_match(name, m, kPattern)) return std::nullopt;
    return std::stoi(m[2].str());
}

} // namespace trace::core
