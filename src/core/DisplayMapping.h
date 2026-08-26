#pragma once

#include <QString>

#include "core/ExrChannels.h"
#include "core/VideoFrame.h"

namespace trace::core {

// HOW SCENE-REFERRED FLOAT PIXELS BECOME SOMETHING A DISPLAY CAN SHOW.
//
// A float frame has no correct 8-bit reading on its own, so one of these is
// ALWAYS in force for one, and which one is ALWAYS reported on screen. That is
// the requirement stated as "a viewer that silently normalises is a viewer I
// cannot trust for review": normalising is allowed, hiding that it happened is
// not.
enum class DisplayMap {
    // The default for a colour pass with no colour transform loaded: clamp to
    // 0..1 and apply a 2.2 display gamma. It is a placeholder for a real view
    // transform and is labelled as such -- the measured beauty pass has 48-66%
    // of its pixels above 1.0, and every one of those clips here.
    Gamma22,
    // A colour pass with the OCIO stage active. Named here so the reported
    // mapping is one field with one vocabulary; the work is done by
    // ColorTransform, not by this file.
    Ocio,
    // A LINEAR STRETCH OVER A FIXED RANGE, for position and depth, where there
    // is no natural range at all. The range is measured ONCE per pass and then
    // PINNED -- see mapFloatToDisplay's `pinned` argument. It used to be
    // re-measured every frame, which is a different mapping on every frame:
    // measured on the Redshift P pass, -44.2500..44.2500 at frame 0 against
    // -39.2500..44.0000 at frame 96, so the same world position was a different
    // grey depending on where the playhead was. A viewer a reviewer cannot
    // trust to show the same number the same way is the thing this whole file
    // exists to avoid. The range in force is always reported.
    Normalise,
    // v * 0.5 + 0.5. The convention for a unit-vector normal pass.
    SignedUnit,
    // Clamp to 0..1 with no gamma. The honest answer for numeric data whose
    // convention we do not recognise.
    Raw,
};

QString displayMapName(DisplayMap map);

// The mapping a pass gets when the user has not chosen otherwise.
DisplayMap defaultMapForClass(PassClass cls);

// The result of running a mapping, for reporting rather than for drawing.
struct DisplayMapResult {
    DisplayMap map = DisplayMap::Gamma22;
    // The input range the mapping worked over. Meaningful for Normalise, where
    // it is what was measured; for the fixed mappings it is the range observed
    // in the frame, which is what says whether anything clipped.
    float inputLo = 0.0f;
    float inputHi = 1.0f;
    // Fraction of samples above 1.0 in the source. Zero for a mapping where
    // that cannot happen. This is the number that says how much a Gamma22
    // preview is throwing away.
    double fractionAboveOne = 0.0;
};

// RGBAF32 in, BGRA8 out. Returns false when `in` is not a float frame, in which
// case `out` is untouched.
//
// Source pixels are never modified, exactly as in ColorTransform::apply and for
// the same reason: the source is very likely still referenced by the frame cache.
// A PINNED INPUT RANGE. Non-null makes Normalise a pure per-pixel function of
// the sample plus two scalars, rather than a whole-frame reduction: stable
// across frames, and expressible as two shader uniforms when the mapping moves
// to the GPU. Ignored by every other mapping, which are already pure.
struct DisplayRange {
    float lo = 0.0f;
    float hi = 1.0f;
    bool valid = false;
};

bool mapFloatToDisplay(const VideoFrame& in, VideoFrame& out, DisplayMap map,
                       DisplayMapResult* result, const DisplayRange* pinned = nullptr);

// Measures the pass's value range and the fraction above 1.0 without producing a
// picture. Used to fill in the reported range for the fixed mappings.
void measureFloatRange(const VideoFrame& in, float& lo, float& hi, double& fractionAboveOne);

} // namespace trace::core
