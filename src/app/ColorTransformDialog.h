#pragma once

#include <QDialog>

#include "core/ColorTransform.h"

class QComboBox;
class QDialogButtonBox;
class QLabel;

namespace trace::app {

// THE Color Transform... DIALOG: config, input colour space, display, view.
//
// It fills in a ColorTransform::Config of Kind::DisplayView and nothing else.
// There is no second pipeline and no second enable flag -- the stage this
// configures is the same stage "Load LUT..." configures, which is the whole
// point of the tagged union in ColorTransform. This dialog is a CALL SITE, as
// stage 1 said it would be.
//
// IT READS AND CANNOT DECIDE. Everything it shows comes from
// ColorTransform's static enumerators, which go through the same config
// resolver setConfig() uses -- so the config named in the combo box and the
// config the processor is built from cannot be different things. That is the
// choke-point property, not a convention observed at call sites.
//
// THE INPUT COLOUR SPACE DEFAULTS TO THE scene_linear ROLE AND NEVER TO THE
// FILE RULES. Measured on both configs in the asset set: the Redshift config's
// file rules answer 'Raw' for .exr where its role is ACEScg, and ocio://default
// answers 'ACES2065-1' where its role is also ACEScg. Every API call succeeds
// either way, so nothing but this rule separates a correct default from a
// wrong-but-plausible one -- and 'ACES2065-1' is the plausible kind, since it
// really is scene-linear and merely has the wrong primaries.
//
// THE TRANSFORM IS APPLIED ON OK, NOT LIVE. OWNER DECISION, 2026-08-24, SETTLED
// -- do not "improve" this into a live preview. Every combo change would
// otherwise recompile an OCIO processor and, on video, issue a decoder Step
// re-request from inside a modal dialog's event loop; the owner declined to
// spend that hazard on a comfort feature, on the stated grounds that the
// see-it/don't-see-it comparison is already served by the `C` bypass rather than
// by this dialog. The reopen condition is his own use annoying him, and nothing
// else.
//
// NO LOOK CONTROL, for the same session's second decision. `Config::look` is
// compiled and reachable and is deliberately left that way -- the same position
// Kind::DisplayView itself was in before stage 3, which the owner named as the
// right amount of readiness. Nothing in the asset set uses a Look.
class ColorTransformDialog : public QDialog {
    Q_OBJECT
public:
    // `current` seeds the controls. A Kind::DisplayView config is restored
    // field for field; anything else (None, or a LUT) starts from the resolved
    // default config with its own scene_linear role and default display/view,
    // which is the state a first-time user should meet.
    ColorTransformDialog(const trace::core::ColorTransform::Config& current,
                         QWidget* parent = nullptr);

    // Valid only after exec() returns Accepted.
    trace::core::ColorTransform::Config result() const;

private:
    void populateConfigs(const QString& preferred);
    // Reloads the three dependent combos from whatever config is selected.
    // `keepSelection` preserves the current input/display/view when the new
    // config still offers them; a config CHANGE passes false, because carrying a
    // colour-space name across configs is how a dialog silently ends up naming a
    // space that does not exist in the config it is about to compile.
    void reloadFromConfig(bool keepSelection);
    void reloadViews(bool keepSelection);
    void browseForConfig();
    void updateStatus(const QString& error);

    QString selectedConfigString() const;

    QComboBox* configCombo_ = nullptr;
    QComboBox* inputCombo_ = nullptr;
    QComboBox* displayCombo_ = nullptr;
    QComboBox* viewCombo_ = nullptr;
    QLabel* statusLabel_ = nullptr;
    QDialogButtonBox* buttons_ = nullptr;

    // The index of the "Browse for a config file..." row, which is an ACTION
    // rather than a value and must never be left selected.
    int browseIndex_ = -1;
    // What was selected before the current change, so choosing Browse and then
    // cancelling the file dialog can put the combo back rather than leaving it
    // sitting on an action row.
    int lastConfigIndex_ = 0;
    bool populating_ = false;
};

} // namespace trace::app
