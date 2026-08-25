#include <QApplication>
#include <QStyleFactory>
#include <QIcon>
#include <QStringList>
#include <QTextStream>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
#endif

#include "app/MainWindow.h"
#include "app/Theme.h"
#include "app/WindowShape.h"
#include "core/ColorTransform.h"
#include "core/ExrChannels.h"
#ifdef TRACE_WITH_OCIO
#include <OpenColorIO/OpenColorIO.h>
#endif

#include "ui/ViewerWidget.h"

namespace {

#ifdef _WIN32
// Trace links as a GUI-subsystem executable now (owner item 18: no console
// window on launch), which means a process started from a terminal has no
// console of its own -- and every diagnostic knob in the tree (TRACE_OPEN_LOG,
// TRACE_SHAPE_LOG, TRACE_SETTINGS_LOG, TRACE_SEEK_LOG, TRACE_LUCID_LOG,
// TRACE_THEME_LOG) writes to stderr with fprintf, as do the two selftests'
// stdout reports. Without this they would all go quiet.
//
// The rule: if a std handle is already VALID, leave it alone -- that is a
// redirection (CI capturing the selftest, a harness capturing stderr) and the
// pipe must keep working. Only when a stream has no handle at all do we attach
// the parent's console and bind the orphaned stream to it. Launched from
// Explorer there is no parent console, AttachConsole fails, and the logs go
// nowhere -- which is the correct behaviour for a double-clicked GUI app.
void attachParentConsoleForDiagnostics() {
    const auto handleValid = [](DWORD which) {
        const HANDLE h = GetStdHandle(which);
        return h != nullptr && h != INVALID_HANDLE_VALUE;
    };
    const bool outValid = handleValid(STD_OUTPUT_HANDLE);
    const bool errValid = handleValid(STD_ERROR_HANDLE);
    if (outValid && errValid) return;
    if (!AttachConsole(ATTACH_PARENT_PROCESS)) return;
    FILE* f = nullptr;
    if (!outValid) freopen_s(&f, "CONOUT$", "w", stdout);
    if (!errValid) freopen_s(&f, "CONOUT$", "w", stderr);
}
#endif

// `Trace.exe --renderer-selftest`: build the viewer, let it adopt the renderer
// TRACE_RENDERER selects, report what is ACTUALLY presenting, and exit.
//
// This exists because the build being green said nothing about the renderer. CI
// compiles, deploys and checks that files are present; it never started the app.
// With d3d11 the default (plan section 25), a build whose GPU backend fails
// initialize() would go green and every user would silently land on the CPU
// fallback -- which is precisely the "a GPU path that quietly never engages
// while the app looks fine" failure the whole renderer boundary was designed
// against (plan section 12), and precisely what the repo's "green must mean
// launchable" rule exists to catch. Plan section 5 asked for this by name.
//
// It runs the real path, not a reduced one: ViewerWidget's constructor applies
// the widget-level native-surface contract and calls initialize(), which creates
// the device, the child surface window, the flip-model swapchain, every shader
// and the render target. A CI runner has no GPU, so the device lands on WARP --
// that is the point of the WARP retry in createDevice(), and "d3d11 (warp)" is a
// pass.
//
// No show(). initialize() reaches the HWND through winId(), which realises the
// native window on its own, and requiring a visible window would make the check
// depend on the runner having an interactive desktop.
//
// `expected` comes from `--renderer-selftest=d3d11` and is matched as a prefix,
// so "d3d11 (warp)" satisfies "d3d11". Empty means report only. The expectation
// lives here rather than as a grep in the workflow because a fallback and a
// backend that was never built produce different exit codes and deserve
// different messages -- a regex over one line cannot tell them apart.
int runRendererSelfTest(const QString& expected) {
    trace::ui::ViewerWidget viewer;

    const QString name = viewer.rendererName();
    const bool fellBack = viewer.rendererFellBack();

    QTextStream out(stdout);
    // One machine-readable line. `planar` is reported because a failed YUV
    // shader is deliberately non-fatal (GATE C) -- the backend keeps presenting
    // BGRA and nothing else says the shader path was lost, which is the same
    // silent-degradation class this check exists for.
    out << "trace-selftest: renderer=" << name
        << " fellback=" << (fellBack ? 1 : 0)
        << " planar=" << (viewer.rendererAcceptsPlanarYuv() ? 1 : 0)
        << Qt::endl;
    out.flush();

    QTextStream err(stderr);
    if (fellBack) {
        err << "trace-selftest: FAIL - the selected renderer failed to "
               "initialize and the cpu backend was adopted instead." << Qt::endl;
        return 3;
    }
    if (!expected.isEmpty() && !name.startsWith(expected)) {
        // Distinct from the fallback case on purpose: nothing failed here, the
        // backend was simply never built into this binary (no fxc, or not a
        // Windows/MSVC build), so createRenderer() never offered it.
        err << "trace-selftest: FAIL - expected renderer '" << expected
            << "' but '" << name << "' is presenting; the backend is not in "
               "this build." << Qt::endl;
        return 4;
    }
    return 0;
}

// `Trace.exe --ocio-selftest[=<file>]`: prove that THIS BINARY links and
// executes OpenColorIO, not merely that CI managed to build the library.
//
// Stage 0 shipped OCIO on the link line and nothing referenced it, so the
// linker emitted no direct import and `TRACE_WITH_OCIO=1` was a claim nothing
// tested -- the same silent-degradation class as a renderer that quietly falls
// back, which is what --renderer-selftest exists for. This is that check for
// the colour stage, and it is deliberately five separate assertions rather than
// one "did it throw", because each can fail on its own and they fail for
// different reasons:
//
//   1. the library is compiled in at all      -> exit 20
//   2. its runtime version can be read        -> exit 21
//   3. a KNOWN config loads                   -> exit 22
//   4. a processor and CPU processor compile  -> exit 23
//   5. the transform actually MOVES A PIXEL   -> exit 24
//
// (5) is the one that matters and the one an "it did not throw" check would
// miss. A processor that compiles and then applies an identity is
// indistinguishable from a working one by every other signal here, and identity
// is exactly what a mis-resolved colour space produces -- the stage-0 measurement
// of getColorSpaceFromFilepath() returning "Raw" for .exr is that failure in the
// wild. So the pixel is compared before and after and the test fails if nothing
// changed.
//
// THE CONFIG IS OCIO'S OWN BUILT-IN ACES CONFIG, not a file on disk. A CI runner
// has no colour configs and no test assets, and a selftest that needs one could
// not run there -- which is the whole point of adding it to CI. `ocio://default`
// is compiled into the library, so this command works anywhere the binary does.
// An optional `=<file>` additionally loads a real config or LUT, for a machine
// that has one.
int runOcioSelfTest(const QString& file) {
    QTextStream out(stdout);
    QTextStream err(stderr);

    if (!trace::core::ColorTransform::available()) {
        err << "trace-ocio: FAIL - this build was compiled without OpenColorIO."
            << Qt::endl;
        return 20;
    }

    const QString version = trace::core::ColorTransform::ocioVersion();
    if (version.isEmpty()) {
        err << "trace-ocio: FAIL - OpenColorIO reported no version." << Qt::endl;
        return 21;
    }

#ifdef TRACE_WITH_OCIO
    std::string display, view, input;
    try {
        auto cfg = OCIO_NAMESPACE::Config::CreateFromBuiltinConfig("ocio://default");
        if (!cfg) {
            err << "trace-ocio: FAIL - the built-in config loaded as null." << Qt::endl;
            return 22;
        }
        // scene_linear rather than the file rules -- the stage-0 finding, applied
        // here so the selftest exercises the same decision the product makes.
        const char* role = cfg->getCanonicalName(OCIO_NAMESPACE::ROLE_SCENE_LINEAR);
        input = (role && *role) ? role : "";
        display = cfg->getDefaultDisplay() ? cfg->getDefaultDisplay() : "";
        view = (!display.empty() && cfg->getDefaultView(display.c_str()))
                   ? cfg->getDefaultView(display.c_str()) : "";
        if (input.empty() || display.empty() || view.empty()) {
            err << "trace-ocio: FAIL - built-in config states no scene_linear "
                   "role or no default display/view." << Qt::endl;
            return 22;
        }

        auto dvt = OCIO_NAMESPACE::DisplayViewTransform::Create();
        dvt->setSrc(input.c_str());
        dvt->setDisplay(display.c_str());
        dvt->setView(view.c_str());

        auto proc = cfg->getProcessor(dvt);
        if (!proc) {
            err << "trace-ocio: FAIL - no processor from the display/view transform."
                << Qt::endl;
            return 23;
        }
        auto cpu = proc->getDefaultCPUProcessor();
        if (!cpu) {
            err << "trace-ocio: FAIL - no CPU processor." << Qt::endl;
            return 23;
        }

        // 18% scene-linear grey through an ACES display transform must not come
        // back as 18% linear. Executed, not assumed.
        float px[3] = {0.18f, 0.18f, 0.18f};
        const float before[3] = {px[0], px[1], px[2]};
        cpu->applyRGB(px);
        const bool moved = (px[0] != before[0]) || (px[1] != before[1]) || (px[2] != before[2]);

        out << "trace-ocio: version=" << version
            << " config=ocio://default"
            << " input=" << QString::fromStdString(input)
            << " display=" << QString::fromStdString(display)
            << " view=" << QString::fromStdString(view)
            << " rgb 0.18->" << QString::number(px[0], 'f', 5)
            << "," << QString::number(px[1], 'f', 5)
            << "," << QString::number(px[2], 'f', 5)
            << " moved=" << (moved ? 1 : 0)
            << Qt::endl;
        out.flush();

        if (!moved) {
            err << "trace-ocio: FAIL - the transform compiled but left the pixel "
                   "unchanged, which is indistinguishable from no transform at all."
                << Qt::endl;
            return 24;
        }
    } catch (const std::exception& e) {
        err << "trace-ocio: FAIL - " << QString::fromUtf8(e.what()) << Qt::endl;
        return 22;
    }
#endif

    // Optional second half: a real file, when one is named. Goes through the
    // PRODUCT's own ColorTransform rather than a second copy of the logic, so a
    // pass here is a statement about the shipping stage.
    if (!file.isEmpty()) {
        trace::core::ColorTransform ct;
        trace::core::ColorTransform::Config cfg;
        cfg.kind = file.endsWith(QStringLiteral(".ocio"), Qt::CaseInsensitive)
                       ? trace::core::ColorTransform::Kind::DisplayView
                       : trace::core::ColorTransform::Kind::Lut;
        if (cfg.kind == trace::core::ColorTransform::Kind::Lut) cfg.lutPath = file;
        else cfg.configPath = file;

        QString error;
        if (!ct.setConfig(cfg, error)) {
            err << "trace-ocio: FAIL - " << error << Qt::endl;
            return 25;
        }
        ct.setEnabled(true);
        out << "trace-ocio: file=" << file
            << " kind=" << (cfg.kind == trace::core::ColorTransform::Kind::Lut ? "lut" : "displayview")
            << " active=" << (ct.isActive() ? 1 : 0)
            << Qt::endl;
        out.flush();
        if (!ct.isActive()) {
            err << "trace-ocio: FAIL - the configuration compiled but the stage "
                   "is not active." << Qt::endl;
            return 25;
        }
    }

    // ---- STAGE 3: config discovery, and the default the dialog depends on ----
    //
    //   6. the built-in registry enumerates, and the default resolves  -> exit 26
    //   7. scene_linear and the FILE RULES give DIFFERENT answers      -> exit 27
    //
    // (6) IS THE CHECK FOR A MEASURED TRAP, NOT A TAUTOLOGY. With $OCIO unset,
    // OCIO::Config::CreateFromEnv() does not throw and does not return null: it
    // returns a "Color management disabled" RAW config with ONE colour space and
    // ONE display, announced only on stderr. Stage 1's DisplayView branch called
    // it whenever configPath was empty. So the assertion is not "a config
    // loaded" -- that would pass on the raw config -- it is that the resolved
    // default carries MORE THAN ONE colour space and at least one display, which
    // the disabled config cannot.
    //
    // (7) ASSERTS A DIFFERENCE RATHER THAN A VALUE, which is what makes it
    // durable. The whole reason the input space defaults to the scene_linear
    // ROLE is that getColorSpaceFromFilepath() answers something else and every
    // API call succeeds either way. Measured: on ocio://default the role is
    // ACEScg and the file rules say ACES2065-1 -- both scene-linear, so the
    // wrong one looks plausible and is merely wrong in its primaries. If a
    // future OCIO ever made the two agree, this fails and says the premise
    // moved, instead of leaving a comment behind that no longer applies.
#ifdef TRACE_WITH_OCIO
    {
        const auto builtins = trace::core::ColorTransform::builtinConfigs();
        trace::core::ColorTransform::ConfigSource src =
            trace::core::ColorTransform::ConfigSource::None;
        const QString defaultCfg =
            trace::core::ColorTransform::defaultConfigString(&src);

        QString e1, e2, e3;
        const QStringList spaces = trace::core::ColorTransform::colorSpaces(QString(), e1);
        const QStringList disp = trace::core::ColorTransform::displays(QString(), e2);
        const QString sceneLinear = trace::core::ColorTransform::sceneLinearSpace(QString(), e3);

        out << "trace-ocio: builtins=" << builtins.size()
            << " default=" << defaultCfg
            << " source=" << (src == trace::core::ColorTransform::ConfigSource::Env
                                  ? "env" : "builtin")
            << " spaces=" << spaces.size()
            << " displays=" << disp.size()
            << " scene_linear=" << sceneLinear
            << Qt::endl;
        out.flush();

        if (builtins.isEmpty()) {
            err << "trace-ocio: FAIL - the built-in config registry enumerated "
                   "nothing, so Trace cannot offer colour management on a machine "
                   "with no config installed." << Qt::endl;
            return 26;
        }
        if (spaces.size() < 2 || disp.isEmpty() || sceneLinear.isEmpty()) {
            err << "trace-ocio: FAIL - the resolved default config has "
                << spaces.size() << " colour space(s) and " << disp.size()
                << " display(s). One of each is the signature of OCIO's "
                   "'Color management disabled' raw config, which is what "
                   "CreateFromEnv() returns when $OCIO is unset." << Qt::endl;
            return 26;
        }

        try {
            auto cfg = OCIO_NAMESPACE::Config::CreateFromFile(
                defaultCfg.toStdString().c_str());
            const char* byRule = cfg ? cfg->getColorSpaceFromFilepath("probe.exr") : nullptr;
            const QString rule = byRule ? QString::fromUtf8(byRule) : QString();
            out << "trace-ocio: input default: scene_linear='" << sceneLinear
                << "' file-rule('.exr')='" << rule << "' differ="
                << (rule != sceneLinear ? 1 : 0) << Qt::endl;
            out.flush();
            if (rule.isEmpty()) {
                err << "trace-ocio: FAIL - getColorSpaceFromFilepath answered "
                       "nothing, so the comparison that justifies defaulting to "
                       "the scene_linear role could not be made." << Qt::endl;
                return 27;
            }
            if (rule == sceneLinear) {
                err << "trace-ocio: FAIL - the file rules and the scene_linear "
                       "role now AGREE on this config. That is not a defect, but "
                       "it means the recorded reason for preferring the role no "
                       "longer holds here and must be re-derived." << Qt::endl;
                return 27;
            }
        } catch (const std::exception& e) {
            err << "trace-ocio: FAIL - " << QString::fromUtf8(e.what()) << Qt::endl;
            return 27;
        }
    }
#endif

    return 0;
}

// `Trace.exe --exr-channels-selftest`: drive the EXR channel grouper over a
// table of synthetic channel lists, print every pass it produces, and fail on
// anything that does not hold.
//
// IT EXISTS BECAUSE THE ASSET SET CONTAINS ONLY TWO OF THE THREE RECORDED
// NAMING CONVENTIONS. Measured with OpenImageIO over every EXR in the pool: the
// root layer spells its components `R G B`, and every Redshift AOV -- including
// that file's own Cryptomatte -- spells them `Beauty.red` / `.green` / `.blue`.
// The third convention, upper-case with alpha (`CryptoMaterial.R/.G/.B/.A`), is
// recorded in CLAUDE.md from stage 0 and has NO FILE HERE to exercise it. A
// grouper written against any one convention finds nothing in the other two, so
// the one that cannot be tested against real media is precisely the one that
// needs a test, and a synthetic channel list is the only way to write it.
//
// It is pure logic over a QStringList: no file, no OpenImageIO, no window. That
// is what lets it run in CI beside the shape and OCIO selftests, and it is also
// the honest limit of what it proves -- that the RULES are right, not that any
// particular file decodes. The file half is covered by opening real media.
//
// THE ASSERTIONS ARE PROPERTIES FIRST AND EXPECTATIONS SECOND. Five invariants
// are checked on every case, including ones with no expectation table, because
// they are the things whose violation is silent:
//
//   1. every channel index in the file appears in at least one pass -- nothing
//      in the file is invisible;
//   2. every stored index is in range;
//   3. a component slot holds a channel whose OWN NAME ends in that component --
//      the check that resolution is by identity and never by position, which is
//      the assumption stage 0 recorded as a live defect;
//   4. no two passes share a display name -- an ambiguity the UI could not
//      express;
//   5. a Colour pass has all three of R, G and B.
static int runExrChannelsSelfTest() {
    using namespace trace::core;
    QTextStream out(stdout);
    int failures = 0;

    struct Expect {
        const char* displayName;
        const char* className;
        const char* rawNames;   // joined with ',' -- empty means "do not check"
    };
    struct Case {
        const char* name;
        QStringList channels;
        std::vector<Expect> expect;   // empty means invariants only
    };

    const std::vector<Case> cases = {
        // The two files in the pool that are a plain render.
        {"root RGB (Beauty_Only, R2_OP_Stacks)",
         {"R", "G", "B"},
         {{"(root)", "colour", "R,G,B"}}},

        {"root RGBA",
         {"R", "G", "B", "A"},
         {{"(root)", "colour", "R,G,B,A"}}},

        // The 27-channel Redshift file, channel for channel as OIIO presents it.
        {"Redshift multilayer, 27 channels",
         {"R", "G", "B",
          "Beauty.red", "Beauty.green", "Beauty.blue",
          "Cryptomatte.red", "Cryptomatte.green", "Cryptomatte.blue",
          "DiffuseFilter.red", "DiffuseFilter.green", "DiffuseFilter.blue",
          "GI.red", "GI.green", "GI.blue",
          "P.red", "P.green", "P.blue",
          "Reflections.red", "Reflections.green", "Reflections.blue",
          "Shadows.red", "Shadows.green", "Shadows.blue",
          "SpecularLighting.red", "SpecularLighting.green", "SpecularLighting.blue"},
         {{"(root)", "colour", "R,G,B"},
          {"Beauty", "colour", "Beauty.red,Beauty.green,Beauty.blue"},
          {"Cryptomatte", "data", "Cryptomatte.red,Cryptomatte.green,Cryptomatte.blue"},
          {"DiffuseFilter", "colour", ""},
          {"GI", "colour", ""},
          {"P", "position", "P.red,P.green,P.blue"},
          {"Reflections", "colour", ""},
          {"Shadows", "colour", ""},
          {"SpecularLighting", "colour", ""}}},

        // CONVENTION 3, which no file here has: upper-case, WITH alpha.
        {"Cryptomatte upper-case with alpha (no file in the pool)",
         {"CryptoMaterial.R", "CryptoMaterial.G", "CryptoMaterial.B", "CryptoMaterial.A"},
         {{"CryptoMaterial", "data",
           "CryptoMaterial.R,CryptoMaterial.G,CryptoMaterial.B,CryptoMaterial.A"}}},

        // Case is not meaningful in a component suffix.
        {"mixed case suffixes",
         {"Beauty.Red", "Beauty.GREEN", "Beauty.blue", "Beauty.Alpha"},
         {{"Beauty", "colour", "Beauty.Red,Beauty.GREEN,Beauty.blue,Beauty.Alpha"}}},

        // THE POSITIONAL ASSUMPTION, WRITTEN AS A TEST. Stored out of order in
        // the file; a grouper resolving by position would put blue in red.
        {"components out of order in the file",
         {"foo.B", "foo.A", "foo.R", "foo.G"},
         {{"foo", "colour", "foo.R,foo.G,foo.B,foo.A"}}},

        // A layer whose name is a class keyword, and the substring trap: neither
        // "Specular" (contains p) nor "Reflections" (contains n) may be caught.
        {"class keywords and the substring trap",
         {"Z", "N.red", "N.green", "N.blue",
          "Specular.red", "Specular.green", "Specular.blue"},
         {{"Z", "depth", "Z,Z,Z"},
          {"N", "normal", "N.red,N.green,N.blue"},
          {"Specular", "colour", "Specular.red,Specular.green,Specular.blue"}}},

        {"nested layer name",
         {"diffuse.light1.R", "diffuse.light1.G", "diffuse.light1.B"},
         {{"diffuse.light1", "colour", "diffuse.light1.R,diffuse.light1.G,diffuse.light1.B"}}},

        // A bare data channel becomes its own pass and is replicated across RGB
        // so it draws as grey rather than as a red-only picture.
        {"bare data channels",
         {"R", "G", "B", "Z", "materialId"},
         {{"(root)", "colour", "R,G,B"},
          {"Z", "depth", "Z,Z,Z"},
          {"materialId", "data", "materialId,materialId,materialId"}}},

        // EDGE CASES WITH NO EXPECTATION TABLE: the invariants alone decide, and
        // these are the shapes most likely to be malformed in the wild.
        // An alpha-only layer replicates alpha across RGB. Without it the loader
        // draws a black frame with an alpha nothing reads: present, selectable
        // and invisible.
        {"alpha-only layer", {"mask.A"},
         {{"mask", "data", "mask.A,mask.A,mask.A,mask.A"}}},

        // A bare `Z` and a `Z.*` layer are DIFFERENT passes. Merged, the three
        // layer channels were dropped entirely.
        {"layer name colliding with a bare channel", {"Z", "Z.R", "Z.G", "Z.B"},
         {{"Z", "depth", "Z,Z,Z"},
          {"Z (layer)", "depth", "Z.R,Z.G,Z.B"}}},

        // The loser of a slot contest is recorded, not discarded.
        {"duplicate component in one layer", {"foo.R", "foo.R", "foo.G", "foo.B"},
         {{"foo", "colour", "foo.R,foo.G,foo.B"}}},
        {"two channels only", {"uv.R", "uv.G"}, {}},
        {"single bare channel", {"Y"}, {}},
    };

    for (const Case& c : cases) {
        const std::vector<ExrPass> passes = groupExrChannels(c.channels);
        out << "-- " << c.name << "  (" << c.channels.size() << " channels -> "
            << static_cast<int>(passes.size()) << " passes)\n";

        for (const ExrPass& p : passes) {
            out << QString("     %1  %2  [%3]  ch %4,%5,%6,%7%8\n")
                       .arg(p.displayName, -22)
                       .arg(passClassName(p.cls), -9)
                       .arg(p.rawNames.join(QLatin1Char(',')))
                       .arg(p.channel[0]).arg(p.channel[1])
                       .arg(p.channel[2]).arg(p.channel[3])
                       .arg(p.ambiguous.isEmpty()
                                ? QString()
                                : QStringLiteral("  ambiguous:")
                                      + p.ambiguous.join(QLatin1Char(',')));
        }

        const auto fail = [&](const QString& why) {
            out << "     FAIL: " << why << "\n";
            ++failures;
        };

        // (1) and (2): nothing in the file is invisible, nothing is out of range.
        std::vector<bool> seen(static_cast<std::size_t>(c.channels.size()), false);
        for (const ExrPass& p : passes) {
            for (int k = 0; k < 4; ++k) {
                const int idx = p.channel[k];
                if (idx < 0) continue;
                if (idx >= c.channels.size()) {
                    fail(QString("pass %1 slot %2 holds out-of-range index %3")
                             .arg(p.displayName).arg(k).arg(idx));
                    continue;
                }
                seen[static_cast<std::size_t>(idx)] = true;
            }
            // A channel recorded as ambiguous is ACCOUNTED FOR: it cannot be
            // shown, because its component slot was already filled, but it has
            // not vanished. Silently disappearing is the failure this invariant
            // exists for; being named as unplaceable is the correct answer for a
            // malformed file.
            for (const QString& amb : p.ambiguous) {
                for (int i = 0; i < c.channels.size(); ++i)
                    if (c.channels.at(i) == amb) seen[static_cast<std::size_t>(i)] = true;
            }
        }
        for (int i = 0; i < c.channels.size(); ++i) {
            if (!seen[static_cast<std::size_t>(i)])
                fail(QString("channel %1 (%2) is in no pass and is not recorded as ambiguous")
                         .arg(i).arg(c.channels.at(i)));
        }

        // (3) RESOLUTION IS BY IDENTITY, NOT POSITION. A slot must hold a
        // channel whose own name ends in that component.
        static const char* kSuffix[4][2] = {{"r", "red"}, {"g", "green"},
                                            {"b", "blue"}, {"a", "alpha"}};
        for (const ExrPass& p : passes) {
            for (int k = 0; k < 4; ++k) {
                const int idx = p.channel[k];
                if (idx < 0 || idx >= c.channels.size()) continue;
                const QString raw = c.channels.at(idx);
                const int dot = raw.lastIndexOf(QLatin1Char('.'));
                const QString suffix = (dot > 0 ? raw.mid(dot + 1) : raw).toLower();
                const bool isComponent = (suffix == QLatin1String(kSuffix[k][0]) ||
                                          suffix == QLatin1String(kSuffix[k][1]));
                // A replicated data channel legitimately sits in all three
                // colour slots under its own name; that is not a mis-resolution.
                const bool replicated = (p.channel[0] == p.channel[1] &&
                                         p.channel[1] == p.channel[2]);
                if (!isComponent && !replicated)
                    fail(QString("pass %1 slot %2 holds \"%3\", whose name is not that component")
                             .arg(p.displayName).arg(k).arg(raw));
            }
        }

        // (4) no two passes share a display name.
        for (std::size_t i = 0; i < passes.size(); ++i)
            for (std::size_t j = i + 1; j < passes.size(); ++j)
                if (passes[i].displayName == passes[j].displayName)
                    fail(QString("two passes share the display name \"%1\"")
                             .arg(passes[i].displayName));

        // (5) a colour pass has all three colour components.
        for (const ExrPass& p : passes) {
            if (p.cls != PassClass::Colour) continue;
            if (p.channel[0] < 0 || p.channel[1] < 0 || p.channel[2] < 0)
                fail(QString("colour pass %1 is missing a component").arg(p.displayName));
        }

        // The expectation table, where there is one.
        if (!c.expect.empty()) {
            if (passes.size() != c.expect.size()) {
                fail(QString("expected %1 passes, got %2")
                         .arg(static_cast<int>(c.expect.size()))
                         .arg(static_cast<int>(passes.size())));
            } else {
                for (std::size_t i = 0; i < passes.size(); ++i) {
                    const Expect& e = c.expect[i];
                    if (passes[i].displayName != QLatin1String(e.displayName))
                        fail(QString("pass %1: expected name \"%2\", got \"%3\"")
                                 .arg(static_cast<int>(i)).arg(e.displayName, passes[i].displayName));
                    if (passClassName(passes[i].cls) != QLatin1String(e.className))
                        fail(QString("pass %1 (%2): expected class %3, got %4")
                                 .arg(static_cast<int>(i)).arg(passes[i].displayName)
                                 .arg(e.className, passClassName(passes[i].cls)));
                    const QString rn = QString::fromLatin1(e.rawNames);
                    if (!rn.isEmpty() && passes[i].rawNames.join(QLatin1Char(',')) != rn)
                        fail(QString("pass %1 (%2): expected raw names \"%3\", got \"%4\"")
                                 .arg(static_cast<int>(i)).arg(passes[i].displayName)
                                 .arg(rn, passes[i].rawNames.join(QLatin1Char(','))));
                }
            }
        }
    }

    if (failures == 0) {
        out << QString("trace-exr-channels: OK - %1 channel layouts\n")
                   .arg(static_cast<int>(cases.size()));
        out.flush();
        return 0;
    }
    out << QString("trace-exr-channels: FAIL - %1 assertions\n").arg(failures);
    out.flush();
    return 5;
}

// `Trace.exe --window-shape-selftest`: drive spec section 4's opening-geometry
// calculation across the aspect matrix at DPR 1.00, 1.25, 1.50 and 2.00, print
// every row, and fail on anything that does not hold.
//
// IT EXISTS BECAUSE THIS MACHINE COULD NOT TEST DPI. Every devicePixelRatioF()
// term in the sizing arithmetic is the identity at 100%, which was the only
// scale factor available here until 2026-08-14, so three quarters of section 4's
// DPI matrix could never execute. computeViewerSize() takes dpr as an argument
// precisely so it can.
//
// IT IS STILL NOT MIXED-MONITOR VALIDATION AND MUST NEVER BE QUOTED AS SUCH. It
// proves the arithmetic and nothing else -- and the hardware pass on 2026-08-14
// is why that distinction is worth keeping rather than a caution: every row here
// PASSED throughout, while the shipping path was failing on real hardware,
// because section 4's sizing never re-ran on a DPI change at all. A pure
// function cannot notice that nobody called it.
//
// The hardware case is now covered by scripts/measure/dpimove.ps1, which needs
// two displays at different scale factors and is therefore not a CI step.
//
// The assertions are properties rather than golden numbers, because a golden
// table would have to be regenerated whenever a policy constant moves and would
// then be asserting whatever the code last did:
//
//   - the returned size matches the requested aspect to within a pixel of
//     rounding -- the one failure that would silently distort the picture;
//   - it never exceeds the area cap, allowing for rounding;
//   - the outer window never exceeds the work area;
//   - it is never narrower than the 460px transport needs;
//   - AND THE DPR ROWS RELATE CORRECTLY, WHICH IS THE CHECK THAT ACTUALLY TESTS
//     DPI. The invariant is NOT "the same logical size at every scale factor",
//     and the first version of this asserted exactly that and failed seven rows
//     on correct code. Which quantity is invariant depends on which rule bound
//     the result, and that is why ShapeBound is reported rather than inferred:
//
//       bound == Natural -- the media is small enough to open 1:1, and "natural
//         displayed size" is a PHYSICAL statement: a 1920-wide source occupies
//         1920 panel pixels, which is 960 logical at 200%. So `logical x dpr`
//         is invariant and equals the source's own pixel size.
//       bound == Cap / WorkArea / Minimum -- all three are expressed in logical
//         pixels (a 1280x720-equivalent area, a fraction of the work area, a
//         460px panel), so the LOGICAL size is invariant.
//
//     A build that multiplies by dpr where it should divide fails both halves:
//     the natural-bound rows come out at dpr^2 of the source size, and the rows
//     that should be capped stop being capped at all.
int runWindowShapeSelfTest() {
    QTextStream out(stdout);
    QTextStream err(stderr);

    struct Media { const char* name; QSize pixels; double aspect; };
    // The full matrix from section 4's validation list, plus the two anamorphic
    // shapes and the rotated one, expressed the way the calculation sees them:
    // a natural displayed size and an on-screen aspect.
    const Media media[] = {
        {"16:9 4K",      QSize(3840, 2160), 16.0 / 9.0},
        {"16:9 1080p",   QSize(1920, 1080), 16.0 / 9.0},
        {"9:16",         QSize(1080, 1920), 9.0 / 16.0},
        {"4:3",          QSize(1440, 1080), 4.0 / 3.0},
        {"1:1",          QSize(1080, 1080), 1.0},
        {"2.39:1",       QSize(2304, 816),  2304.0 / 816.0},
        {"4:5",          QSize(1080, 1350), 0.8},
        {"anamorphic",   QSize(1920, 1080), 16.0 / 9.0},
        {"rot90 of 16:9",QSize(1080, 1920), 9.0 / 16.0},
        {"very small",   QSize(320, 240),   4.0 / 3.0},
        {"8K",           QSize(7680, 4320), 16.0 / 9.0},
    };
    const double dprs[] = {1.00, 1.25, 1.50, 2.00};

    int failures = 0;
    for (const Media& m : media) {
        QSize atDpr1;
        for (double dpr : dprs) {
            trace::app::ShapeInputs in;
            in.naturalPixels = m.pixels;
            in.aspect = m.aspect;
            in.dpr = dpr;
            // Representative, and fixed across the matrix so the rows are
            // comparable: measured chrome on this box is 0x407 with the HUD
            // shown, and the frame 0x31.
            in.chromeLogical = QSize(0, 407);
            in.frameLogical = QSize(0, 31);
            in.workAreaLogical = QSize(2560, 1400);
            // The aspect-correct floor MainWindow hands it.
            in.viewerMinimumLogical =
                m.aspect >= 1.0 ? QSize(static_cast<int>(std::lround(360.0 * m.aspect)), 360)
                                : QSize(360, static_cast<int>(std::lround(360.0 / m.aspect)));

            const trace::app::ShapeResult r = trace::app::computeViewerSize(in);
            if (!r.valid) {
                err << "trace-shape: FAIL - " << m.name << " @ dpr " << dpr
                    << " produced no result" << Qt::endl;
                ++failures;
                continue;
            }

            const double gotAspect =
                static_cast<double>(r.viewerLogical.width()) / r.viewerLogical.height();
            out << QString("  %1  dpr %2  ->  %3x%4  aspect %5  scale %6  bound %7")
                       .arg(QString::fromLatin1(m.name), -16)
                       .arg(dpr, 0, 'f', 2)
                       .arg(r.viewerLogical.width(), 5)
                       .arg(r.viewerLogical.height(), -5)
                       .arg(gotAspect, 0, 'f', 4)
                       .arg(r.scale, 0, 'f', 4)
                       .arg(QString::fromLatin1(trace::app::shapeBoundName(r.bound)))
                << Qt::endl;

            // Aspect preserved. One pixel of tolerance on the height, which is
            // all the rounding of a single lround can introduce.
            const int wantH = static_cast<int>(std::lround(r.viewerLogical.width() / m.aspect));
            if (std::abs(r.viewerLogical.height() - wantH) > 1) {
                err << "trace-shape: FAIL - " << m.name << " @ dpr " << dpr
                    << " distorted the ratio: got " << r.viewerLogical.height()
                    << " want " << wantH << Qt::endl;
                ++failures;
            }
            // The area cap, unless the minimum had to push past it.
            const double area = static_cast<double>(r.viewerLogical.width()) * r.viewerLogical.height();
            if (r.bound != trace::app::ShapeBound::Minimum && area > in.capAreaLogical * 1.01) {
                err << "trace-shape: FAIL - " << m.name << " @ dpr " << dpr
                    << " exceeded the area cap: " << area << Qt::endl;
                ++failures;
            }
            // Never off the monitor.
            if (r.viewerLogical.width() + in.chromeLogical.width() + in.frameLogical.width()
                    > in.workAreaLogical.width()
                || r.viewerLogical.height() + in.chromeLogical.height() + in.frameLogical.height()
                    > in.workAreaLogical.height()) {
                err << "trace-shape: FAIL - " << m.name << " @ dpr " << dpr
                    << " exceeded the work area" << Qt::endl;
                ++failures;
            }
            // The settled 460px transport fits.
            if (r.viewerLogical.width() < in.minTransportWidthLogical) {
                err << "trace-shape: FAIL - " << m.name << " @ dpr " << dpr
                    << " is narrower than the 460px transport: "
                    << r.viewerLogical.width() << Qt::endl;
                ++failures;
            }

            // THE DPI CHECK, in the two forms the header explains.
            if (r.bound == trace::app::ShapeBound::Natural) {
                // Physical size is the source's own, whatever the scale factor.
                const int physW = static_cast<int>(std::lround(r.viewerLogical.width() * dpr));
                if (std::abs(physW - m.pixels.width()) > 2) {
                    err << "trace-shape: FAIL - " << m.name << " @ dpr " << dpr
                        << " opened at natural size but occupies " << physW
                        << " device px, not the source's " << m.pixels.width()
                        << "; natural size is a physical statement." << Qt::endl;
                    ++failures;
                }
            } else if (!atDpr1.isEmpty() && r.viewerLogical != atDpr1) {
                // Cap, work area and the transport minimum are all logical, so
                // the logical result must not move with the scale factor.
                err << "trace-shape: FAIL - " << m.name << " @ dpr " << dpr
                    << " is " << trace::app::shapeBoundName(r.bound) << "-bound and gave "
                    << r.viewerLogical.width() << "x" << r.viewerLogical.height()
                    << " but dpr 1.00 gave " << atDpr1.width() << "x" << atDpr1.height()
                    << "; a logical constraint must not depend on the scale factor."
                    << Qt::endl;
                ++failures;
            }
            if (dpr == 1.00 && r.bound != trace::app::ShapeBound::Natural) atDpr1 = r.viewerLogical;
        }
    }

    if (failures > 0) {
        err << "trace-shape: " << failures << " failure(s)" << Qt::endl;
        return 5;
    }
    out << "trace-shape: OK - " << (sizeof(media) / sizeof(media[0])) << " shapes x "
        << (sizeof(dprs) / sizeof(dprs[0])) << " scale factors" << Qt::endl;
    // THE CAVEAT TRAVELS WITH THE RESULT, and it is narrower than it used to be
    // rather than gone. Real mixed-monitor DPI was validated on hardware on
    // 2026-08-14 (plan 20.4) -- but by scripts/measure/dpimove.ps1, not by this,
    // and this passed every row on the build where that pass found section 4's
    // sizing never re-running on a DPI change.
    out << "trace-shape: NOTE - synthetic DPR only: this proves the ARITHMETIC. "
           "The hardware path (WM_DPICHANGED, swapchain resize, monitor-to-monitor "
           "moves) is scripts/measure/dpimove.ps1 and needs two displays at "
           "different scale factors." << Qt::endl;
    return 0;
}

} // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Before anything can print: the selftests below report on stdout and the
    // TRACE_*_LOG knobs on stderr, and in a GUI-subsystem process neither
    // stream exists until it is bound to a console.
    attachParentConsoleForDiagnostics();
#endif
    QApplication app(argc, argv);
    app.setApplicationName("Trace");
    app.setOrganizationName("Trace Project");
    // UI redesign roadmap step 10: typography, the palette and the popup-menu
    // surface, in one place. Before any window is built, and before the
    // self-tests below return -- the shape self-test measures nothing that
    // depends on it, but the font is an application-wide default and a window
    // constructed ahead of it would carry the old one. The style (Fusion,
    // wrapped for the mnemonic-underline setting, owner item 10) moved in
    // there too, so appearance has one home rather than two.
    trace::app::Theme::apply(app);

    // Before any window: the self-test wants the renderer and nothing else, and
    // it must not open media, start a clock or touch an audio device.
    for (const QString& arg : app.arguments()) {
        if (!arg.startsWith(QStringLiteral("--renderer-selftest"))) continue;
        const qsizetype eq = arg.indexOf(QLatin1Char('='));
        return runRendererSelfTest(eq < 0 ? QString() : arg.mid(eq + 1));
    }

    // Needs no widget, no renderer and no window -- and no colour config on
    // disk either, which is what lets CI run it.
    for (const QString& arg : app.arguments()) {
        if (!arg.startsWith(QStringLiteral("--ocio-selftest"))) continue;
        const qsizetype eq = arg.indexOf(QLatin1Char('='));
        return runOcioSelfTest(eq < 0 ? QString() : arg.mid(eq + 1));
    }

    // Pure arithmetic: no widget, no renderer, no window. It runs anywhere the
    // binary does, which is what lets CI check the DPI matrix this machine
    // cannot.
    for (const QString& arg : app.arguments()) {
        if (arg == QStringLiteral("--window-shape-selftest")) return runWindowShapeSelfTest();
    }

    // Pure logic over a channel-name list: no file, no OpenImageIO, no window.
    // Runs anywhere the binary does, which is what lets CI check the naming
    // convention the asset set has no file for.
    for (const QString& arg : app.arguments()) {
        if (arg == QStringLiteral("--exr-channels-selftest")) return runExrChannelsSelfTest();
    }

    // `Trace.exe --scrub-selftest=<clip>` (or `--scrub-selftest <clip>`): the
    // scrub diagnostic. Unlike the two selftests above it needs the FULL
    // application -- the drag it scripts runs through MainWindow's real slider,
    // coalescing timer, worker lease and landing, because a reduced harness
    // would measure a path nobody scrubs on. It therefore builds and shows the
    // real window, drives the gesture itself, prints one pasteable block to
    // stdout AND writes trace-scrub-report.txt beside the exe, then exits.
    //
    // It exists for the machine-dependent scrub report (2026-08-19,
    // docs/mp4-scrub-threadripper.md): the affected machine is locked down, so
    // the discriminating numbers -- above all the worker round trip, split from
    // decode -- have to come from one command anyone can run. NOT a CI step:
    // it needs a real clip and a real desktop, and its numbers only mean
    // anything relative to another machine's run of the same command.
    {
        const QStringList args = app.arguments();
        for (qsizetype i = 0; i < args.size(); ++i) {
            const QString& arg = args.at(i);
            if (!arg.startsWith(QStringLiteral("--scrub-selftest"))) continue;
            const qsizetype eq = arg.indexOf(QLatin1Char('='));
            QString clip = eq < 0 ? QString() : arg.mid(eq + 1);
            // Space-separated form, so a quoted path does not have to share
            // quotes with the option: Trace.exe --scrub-selftest "C:\a b\c.mp4"
            if (clip.isEmpty() && i + 1 < args.size()) clip = args.at(i + 1);
            trace::app::MainWindow win;
            win.show();
            return win.runScrubSelfTest(clip);
        }
    }

    QIcon appIcon;
    appIcon.addFile(QStringLiteral(":/icons/trace-16.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-32.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-48.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-256.png"));
    app.setWindowIcon(appIcon);

    trace::app::MainWindow win;
    win.setWindowIcon(appIcon);
    win.resize(1280, 760);
    win.show();

    // Optional media path: Trace.exe "D:\Media\Clip.mov". Applied after show()
    // so the window exists to display it. Anything unusable is ignored, so a
    // bad argument still leaves a normally running app.
    const QStringList args = app.arguments();
    if (args.size() > 1) {
        win.openMediaPath(args.at(1));
    }

    return app.exec();
}
