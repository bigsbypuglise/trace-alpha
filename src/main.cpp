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

    // Pure arithmetic: no widget, no renderer, no window. It runs anywhere the
    // binary does, which is what lets CI check the DPI matrix this machine
    // cannot.
    for (const QString& arg : app.arguments()) {
        if (arg == QStringLiteral("--window-shape-selftest")) return runWindowShapeSelfTest();
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
