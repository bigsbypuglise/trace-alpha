#pragma once

#include <QString>
#include <QStringList>
#include <vector>

namespace trace::core {

// WHAT KIND OF DATA A PASS CARRIES, DECIDED PER CLASS AND NEVER PER PASS NAME.
//
// The display mapping is chosen from this, not from the pass's own name, because
// the set of names is open (every renderer spells its AOVs differently) while the
// set of *kinds* is small and closed. A name we have never met falls into Data
// and is shown raw and labelled, rather than being silently normalised into
// something that looks plausible.
enum class PassClass {
    Colour,      // scene-referred RGB: beauty, GI, diffuse, specular, reflections
    Position,    // world/object position: signed and unbounded (measured on the
                 // Redshift P pass: -44.25 .. +44.25)
    Normal,      // unit vector, nominally -1 .. +1
    Depth,       // distance from camera; one channel, 0 .. inf
    Data,        // numeric with no convention we recognise -- shown raw
};

QString passClassName(PassClass cls);

// One displayable group of channels.
//
// `channel[]` is in R,G,B,A order and holds INDICES INTO THE FILE'S CHANNEL
// LIST, which is not the same thing as the file's own order: OpenImageIO
// presents the Redshift file as R,G,B then Beauty.red,Beauty.green,Beauty.blue,
// so position within a layer is reliable there and is NOT relied on here -- the
// mapping is by channel NAME in every case.
struct ExrPass {
    QString layer;        // "" for the root group
    QString displayName;  // "(root)" for the root group, else the layer name
    int channel[4] = {-1, -1, -1, -1};   // R, G, B, A; -1 when absent
    QStringList rawNames;                // the file's own names, R,G,B,A order,
                                         // so a convention we have not met is
                                         // visible in the HUD rather than silent
    PassClass cls = PassClass::Colour;

    // Measured at open by comparing a band of scanlines, never assumed from the
    // name. Empty when this pass stands alone.
    QString duplicateOf;

    // A channel with NO component suffix, made into its own pass (Z, materialId).
    // Held so the grouper never merges one into a layer group that happens to
    // share its name: a file carrying both `Z` and `Z.R/.G/.B` would otherwise
    // drop three channels on the floor, which the selftest caught.
    bool standalone = false;

    // RAW NAMES THIS PASS COULD NOT PLACE, kept rather than discarded.
    //
    // A component slot is filled once: two channels claiming the same slot is a
    // malformed file, and taking the later one would silently change which
    // pixels are shown. Keeping the FIRST is the safe half; recording what was
    // dropped is the other half, because a channel that vanishes with no trace
    // is exactly the failure nobody notices. Surfaced on the HUD.
    QStringList ambiguous;

    int channelCount() const {
        int n = 0;
        for (int i = 0; i < 4; ++i)
            if (channel[i] >= 0) ++n;
        return n;
    }
    bool hasAlpha() const { return channel[3] >= 0; }

    // The contiguous span of file channel indices this pass touches. Reading is
    // done over the span rather than per channel, because OIIO's read API takes
    // a channel RANGE -- and the span is what makes reading one pass of a
    // 27-channel file cost 3 channels rather than 27.
    int spanBegin() const;
    int spanEnd() const;   // exclusive
};

// THE GROUPER. Case-insensitive on a suffix set of
// {R, red, G, green, B, blue, A, alpha}, split at the LAST '.'.
//
// Three naming conventions live in one asset set and a grouper written against
// any one of them finds nothing in the other two (measured in stage 0):
//   root layer         R / G / B
//   Redshift AOVs      Beauty.red / Beauty.green / Beauty.blue     (lower-case)
//   Cryptomatte        CryptoMaterial.R / .G / .B / .A             (upper, +A)
//
// Anything whose suffix is not in the set becomes its OWN single-channel pass
// rather than being dropped, so no channel in the file is invisible.
// Pass order follows first appearance in the file.
std::vector<ExrPass> groupExrChannels(const QStringList& channelNames);

// Class from the layer name and the channel count. Exposed so it can be tested
// on its own and so the loader and the UI cannot disagree about a pass's kind.
PassClass classifyPass(const QString& layer, const QString& firstRawName, int channelCount);

} // namespace trace::core
