#pragma once

#include <QImage>
#include <QString>
#include <QStringList>

#include <memory>
#include <vector>

#include "core/ExrChannels.h"
#include "core/VideoFrame.h"

namespace trace::core {

struct LoadedImageInfo {
    QString filePath;
    QString fileName;
    QString extension;
    int width = 0;
    int height = 0;
    // THE FILE'S OWN CHANNEL COUNT, not the display buffer's. The two are
    // different numbers and the HUD used to print the second while claiming the
    // first: it read `ch:4` for a 3-channel EXR because the handoff path
    // hard-coded 4, which was honest about the buffer and misleading as a label.
    int channels = 0;

    // The pixels, always. One field rather than a QImage plus a buffer, because
    // two of those is two sources of truth about what was loaded.
    //
    // BGRA8 for everything QImage reads. RGBAF32 for EXR -- scene-referred, not
    // clamped, and NOT display-ready until a mapping has been chosen for it.
    std::shared_ptr<FrameBuffer> buffer;

    // EXR only, empty otherwise.
    std::vector<ExrPass> passes;
    int activePass = -1;              // index into `passes`
    QString compression;              // "piz", "dwaa", ... -- DWAA is lossy and
                                      // is worth saying out loud before someone
                                      // debugs a compression artefact as a bug
    QStringList activeRawNames;       // the file's own channel names for the
                                      // active pass, in R,G,B,A order

    bool isFloat() const { return buffer && isFloatRgba(buffer->layout()); }
    const ExrPass* activePassInfo() const {
        if (activePass < 0 || activePass >= static_cast<int>(passes.size())) return nullptr;
        return &passes[static_cast<std::size_t>(activePass)];
    }
};

class StillImageLoader {
public:
    // WHICH PASS THE NEXT LOAD SHOULD SHOW.
    //
    // It lives on the loader rather than being an argument because a sequence
    // loads through this object one frame at a time, and the pass has to be the
    // same for every frame of it -- an argument would have to be threaded
    // through ImageSequenceFrameSource, the prefetcher and the frame cache, and
    // any one of them missing it would change pass mid-playback.
    //
    // Empty means "the file decides": the root group if it has one, else the
    // first colour pass. A name that is not in a given file falls back the same
    // way rather than failing, so a sequence with an inconsistent header still
    // plays.
    void setPreferredPass(const QString& layer) { preferredPass_ = layer; }
    QString preferredPass() const { return preferredPass_; }

    bool load(const QString& path, LoadedImageInfo& out, QString& error) const;

private:
    bool loadExr(const QString& path, LoadedImageInfo& out, QString& error) const;

    QString preferredPass_;

    // DUPLICATE PASSES, MEASURED ONCE PER CHANNEL LAYOUT RATHER THAN PER FRAME.
    //
    // Keyed on the file's joined channel names, which is constant across every
    // frame of a sequence -- so a 97-frame sequence pays the comparison on its
    // first frame and never again. The relationship is taken from that frame and
    // asserted for the rest, which is what "measured at open" means and is
    // stated on the HUD rather than implied.
    mutable QString duplicateKey_;
    mutable QStringList duplicateLabels_;   // parallel to the pass list
};

} // namespace trace::core
