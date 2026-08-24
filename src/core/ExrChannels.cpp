#include "core/ExrChannels.h"

#include <algorithm>

namespace trace::core {
namespace {

// The suffix set, lower-cased. Both the short and the long spelling of every
// component, because the asset set contains both and a grouper that knows only
// one reports a multilayer render as a plain RGB image.
int componentIndex(const QString& suffixLower) {
    if (suffixLower == QLatin1String("r") || suffixLower == QLatin1String("red")) return 0;
    if (suffixLower == QLatin1String("g") || suffixLower == QLatin1String("green")) return 1;
    if (suffixLower == QLatin1String("b") || suffixLower == QLatin1String("blue")) return 2;
    if (suffixLower == QLatin1String("a") || suffixLower == QLatin1String("alpha")) return 3;
    return -1;
}

bool isOneOf(const QString& lower, std::initializer_list<const char*> names) {
    for (const char* n : names)
        if (lower == QLatin1String(n)) return true;
    return false;
}

} // namespace

QString passClassName(PassClass cls) {
    switch (cls) {
        case PassClass::Colour:   return QStringLiteral("colour");
        case PassClass::Position: return QStringLiteral("position");
        case PassClass::Normal:   return QStringLiteral("normal");
        case PassClass::Depth:    return QStringLiteral("depth");
        case PassClass::Data:     return QStringLiteral("data");
    }
    return QStringLiteral("data");
}

int ExrPass::spanBegin() const {
    int lo = -1;
    for (int i = 0; i < 4; ++i) {
        if (channel[i] < 0) continue;
        if (lo < 0 || channel[i] < lo) lo = channel[i];
    }
    return lo;
}

int ExrPass::spanEnd() const {
    int hi = -1;
    for (int i = 0; i < 4; ++i)
        hi = std::max(hi, channel[i]);
    return hi + 1;
}

// EXACT NAME MATCHES, NOT SUBSTRINGS. "Specular" contains a p and "Reflections"
// contains an n; a contains() test would classify both as data passes and show
// them through the wrong mapping. A name that is not on a list is Colour when it
// has three channels and Data when it does not -- which is the conservative
// answer in both directions.
PassClass classifyPass(const QString& layer, const QString& firstRawName, int channelCount) {
    const QString n = (layer.isEmpty() ? firstRawName : layer).toLower();

    // Cryptomatte is stage 4. Until then its channels are numeric IDs that
    // happen to sit in RGB slots, so they are Data and are shown raw -- not
    // Colour, which would put a view transform over an ID.
    if (n.startsWith(QLatin1String("crypto"))) return PassClass::Data;

    if (isOneOf(n, {"p", "position", "worldposition", "world_position", "pworld",
                    "pref", "refpos", "point"}))
        return PassClass::Position;
    if (isOneOf(n, {"n", "normal", "normals", "worldnormal", "world_normal",
                    "shadingnormal", "geonormal", "nworld"}))
        return PassClass::Normal;
    if (isOneOf(n, {"z", "depth", "zdepth", "z_depth", "distance"}))
        return PassClass::Depth;

    if (channelCount >= 3) return PassClass::Colour;
    // One or two channels with no known name: numeric, and saying so is better
    // than tinting it and calling it colour.
    return PassClass::Data;
}

std::vector<ExrPass> groupExrChannels(const QStringList& channelNames) {
    std::vector<ExrPass> passes;
    // Layer name -> index into `passes`, so first appearance decides order.
    std::vector<QString> order;

    auto findPass = [&](const QString& layer) -> ExrPass* {
        for (auto& p : passes)
            if (p.layer == layer) return &p;
        return nullptr;
    };

    for (int i = 0; i < channelNames.size(); ++i) {
        const QString& raw = channelNames.at(i);
        const int dot = raw.lastIndexOf(QLatin1Char('.'));
        const QString layer = dot > 0 ? raw.left(dot) : QString();
        const QString suffix = dot > 0 ? raw.mid(dot + 1) : raw;
        const int comp = componentIndex(suffix.toLower());

        if (comp < 0) {
            // Not a colour component. Its own single-channel pass, keyed on the
            // FULL raw name so two unrecognised channels in one layer cannot
            // collide, and replicated across R,G,B so it draws as grey rather
            // than as a red-only picture.
            ExrPass p;
            p.layer = raw;
            p.displayName = raw;
            p.channel[0] = p.channel[1] = p.channel[2] = i;
            p.rawNames = QStringList{raw, raw, raw};
            p.cls = classifyPass(dot > 0 ? layer : QString(), raw, 1);
            passes.push_back(p);
            continue;
        }

        ExrPass* p = findPass(layer);
        if (!p) {
            ExrPass fresh;
            fresh.layer = layer;
            fresh.displayName = layer.isEmpty() ? QStringLiteral("(root)") : layer;
            fresh.rawNames = QStringList{QString(), QString(), QString(), QString()};
            passes.push_back(fresh);
            p = &passes.back();
        }
        // A duplicate component in one layer keeps the FIRST. Two channels
        // claiming the same slot is a malformed file, and taking the later one
        // would silently change which pixels are shown.
        if (p->channel[comp] < 0) {
            p->channel[comp] = i;
            p->rawNames[comp] = raw;
        }
    }

    for (auto& p : passes) {
        if (p.rawNames.size() == 4) {
            // Trim the trailing empty alpha slot so the HUD prints what the file
            // has rather than an empty field.
            while (!p.rawNames.isEmpty() && p.rawNames.last().isEmpty())
                p.rawNames.removeLast();
        }
        if (p.cls == PassClass::Colour)
            p.cls = classifyPass(p.layer, p.rawNames.isEmpty() ? QString() : p.rawNames.first(),
                                 p.channelCount());
        // A single-component colour group (a lone "R", say) is grey data, not a
        // colour picture. Replicate it so it is at least visible.
        if (p.channelCount() == 1 && p.channel[0] >= 0 && p.channel[1] < 0 && p.channel[2] < 0) {
            p.channel[1] = p.channel[2] = p.channel[0];
        }
    }

    return passes;
}

} // namespace trace::core
