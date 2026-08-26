#pragma once

#include <QList>
#include <QString>
#include <QStringList>
#include <memory>

#include "core/VideoFrame.h"

namespace trace::core {

// THE DISPLAY TRANSFORM STAGE. THERE IS ONLY EVER ONE OF THESE.
//
// A LUT is not a separate feature from OCIO -- a .cube IS an OCIO FileTransform,
// needing no config, and the same FileTransform also reads .3dl, .csp, .spi1d,
// .spi3d, .clf and .ctf. So "Load LUT..." is this stage holding a Lut config and
// "Color Transform..." is this stage holding a DisplayView config. Nothing
// downstream knows which: everything past setConfig() sees only a compiled
// processor. That is what the assessment meant by "there is only ever one stage,
// and it is OCIO from the first commit" -- nothing temporary gets written now,
// so nothing gets rewritten when ACES arrives.
//
// The state is exactly the owner's:
//     enabled (bypass) + active transform configuration = displayed result
// and the two are INDEPENDENT. Disabling does not discard the configuration,
// which is what makes re-enabling instant and what makes the checkbox a bypass
// rather than a delete.
class ColorTransform {
public:
    // The tagged union. Adding a kind means adding a compile branch and nothing
    // else -- no second pipeline, no second enable flag, no second menu path.
    enum class Kind {
        None,        // raw: no transform. The reset state.
        Lut,         // an OCIO FileTransform over a LUT file on disk.
        DisplayView, // config + input colour space + display + view (+ look).
    };

    struct Config {
        Kind kind = Kind::None;

        // Kind::Lut
        QString lutPath;

        // Kind::DisplayView. Present now, unused by any UI in stage 1 --
        // deliberately: the stage must be able to hold this from the first
        // commit so the dialog that fills it in is a call site rather than a
        // redesign. The ACEScg default comes from the config's scene_linear
        // ROLE and never from Config::getColorSpaceFromFilepath(), which
        // answers "Raw" for .exr under the Redshift config's own file rules.
        QString configPath;
        QString inputSpace;
        QString display;
        QString view;
        QString look;

        bool operator==(const Config& o) const {
            return kind == o.kind && lutPath == o.lutPath
                && configPath == o.configPath && inputSpace == o.inputSpace
                && display == o.display && view == o.view && look == o.look;
        }
        bool operator!=(const Config& o) const { return !(*this == o); }
    };

    // WHERE THE CONFIG CAME FROM. Reported rather than inferred, for the reason
    // the HUD's `renderer`, `planar`, `font` and `strip` fields exist: the
    // answer is not decided by the command line, so a run whose config resolved
    // somewhere unexpected must SAY so rather than be reasoned about after the
    // fact. It is also the FFmpeg-root lesson -- print the RESOLVED thing, never
    // an echo of the request, because an echo is a claim that cannot fail.
    enum class ConfigSource {
        None,     // no DisplayView configuration compiled
        Builtin,  // one of OCIO's compiled-in ocio:// configs
        Env,      // the file $OCIO pointed at
        File,     // a config the user chose
    };

    // The built-in configs this OCIO carries, newest/recommended first.
    // MEASURED, not assumed: this build reports EIGHT, of which two are flagged
    // recommended. There is no getDefaultBuiltinConfigName() on the registry --
    // `ocio://default` is resolved by the Config factory instead -- so the
    // caller cannot ask which one is the default and this list carries the URI.
    struct BuiltinConfig {
        QString uri;        // "ocio://cg-config-v4.0.0_aces-v2.0_ocio-v2.5"
        QString label;      // the registry's own UI name
        bool recommended = false;
    };
    static QList<BuiltinConfig> builtinConfigs();

    // The config string that would be used when a DisplayView configuration
    // leaves `configPath` empty, and where it came from.
    //
    // THIS IS NOT Config::CreateFromEnv(), AND THE DIFFERENCE IS A MEASURED
    // TRAP. With $OCIO unset, CreateFromEnv() does not throw and does not
    // return null: it returns a "Color management disabled" RAW config with
    // ONE colour space and ONE display, and prints an info line to stderr that
    // nothing in a GUI ever shows. A dialog that offered that as "the
    // environment config" would look like it had loaded something. So $OCIO is
    // used ONLY when it is actually set, and the fallback is a built-in config,
    // which is also what makes Trace work with no colour management installed.
    static QString defaultConfigString(ConfigSource* source = nullptr);

    ColorTransform();
    ~ColorTransform();
    ColorTransform(const ColorTransform&) = delete;
    ColorTransform& operator=(const ColorTransform&) = delete;

    // Was this build compiled with OCIO at all. False makes every setConfig()
    // fail with a reason rather than silently doing nothing.
    static bool available();
    // The OCIO runtime version, read off the library rather than a build
    // constant, or empty when unavailable.
    static QString ocioVersion();

    // BYPASS. Independent of the configuration by design: toggling this must
    // never touch config_ or the compiled processor, so re-enabling is a
    // pointer becoming non-null again and costs nothing.
    bool enabled() const { return enabled_; }
    void setEnabled(bool on) { enabled_ = on; }

    const Config& config() const { return config_; }

    // Compiles the configuration into a processor. Returns false and leaves the
    // PREVIOUS configuration in force on failure -- a bad LUT must not silently
    // become "no transform", because that looks identical to a transform that
    // loaded and did nothing.
    bool setConfig(const Config& config, QString& error);

    // Back to the raw/default state: no transform, bypass off. Defined as one
    // thing so "Reset" cannot mean something subtly different from "never
    // configured".
    void reset();

    // The only question the rest of the application asks. False whenever the
    // stage would be a no-op, so every caller's fast path is a single bool.
    bool isActive() const { return enabled_ && hasProcessor(); }

    // True when a configuration is loaded, whether or not it is enabled. This
    // is what lets the UI say "bypassed" rather than "none".
    bool hasProcessor() const;

    // Short human-readable description of the active configuration, for the HUD
    // and the transient message. Empty for Kind::None.
    QString description() const;

    // Which config the COMPILED processor was actually built from, and its
    // human-readable name. Both are set by setConfig() from what it resolved,
    // never from what it was asked for -- so an empty `configPath` that fell
    // back to a built-in says `built-in ...` here rather than nothing.
    ConfigSource configSource() const { return configSource_; }
    QString configLabel() const { return configLabel_; }

    // Enumerate what a config offers, for the dialog. Each returns empty and
    // sets `error` rather than throwing, because every one of them is reached
    // from a combo box changing.
    static QStringList colorSpaces(const QString& configString, QString& error);
    static QStringList displays(const QString& configString, QString& error);
    static QStringList views(const QString& configString, const QString& display,
                             QString& error);
    // The scene_linear ROLE, which is what an input colour space must default to.
    static QString sceneLinearSpace(const QString& configString, QString& error);
    // The config's own default display, and that display's default view.
    static QString defaultDisplay(const QString& configString, QString& error);
    static QString defaultView(const QString& configString, const QString& display,
                               QString& error);

    // True when a float source can be transformed. Separate from
    // hasProcessor() because the float processor is built beside the 8-bit one
    // and either could in principle fail on its own; a caller that asks the
    // wrong question would silently show an untransformed EXR.
    bool hasFloatProcessor() const;

    // THE STAGE ITSELF. `in` must be BGRA8 or RGBAF32; `out` receives a NEW
    // buffer holding the transformed pixels, ALWAYS BGRA8. Returns false when
    // not active or when `in` is a layout this cannot take (planar YUV), in
    // which case `out` is untouched and the caller displays `in`.
    //
    // THE FLOAT INPUT IS THE POINT OF THE EXR PATH. A scene-linear EXR carries
    // values well above 1.0 -- measured on the Redshift beauty pass, 48-66% of
    // samples -- so flattening to 8 bits before this stage handed the view
    // transform a picture whose highlights had already been clipped. Float in,
    // 8-bit out means the clip happens at the END of the chain, where the
    // display actually imposes it, instead of at the start.
    //
    // SOURCE PIXELS ARE NEVER MODIFIED. `in`'s buffer is read-only here, which
    // is what makes the whole stage a display stage rather than a decode stage
    // -- and it is not merely tidy: that buffer is very likely still referenced
    // by the frame cache and by the decoder's recycling pool.
    //
    // (Until stage 2 this was also what kept Copy Frame copying the SOURCE. It
    // no longer is: Copy Frame reads the displayed buffer now, by owner
    // decision. The read-only property stands on its own reason.)
    bool apply(const VideoFrame& in, VideoFrame& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;   // holds the OCIO objects; opaque so this
                                   // header carries no OCIO include and can be
                                   // reached from a build without it.
    Config config_;
    bool enabled_ = false;
    ConfigSource configSource_ = ConfigSource::None;
    QString configLabel_;
};

} // namespace trace::core
