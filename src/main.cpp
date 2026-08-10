#include <QApplication>
#include <QStyleFactory>
#include <QIcon>
#include <QStringList>
#include <QTextStream>
#include "app/MainWindow.h"
#include "ui/ViewerWidget.h"

namespace {

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

} // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName("Trace");
    app.setOrganizationName("Trace Project");
    app.setStyle(QStyleFactory::create("Fusion"));

    // Before any window: the self-test wants the renderer and nothing else, and
    // it must not open media, start a clock or touch an audio device.
    for (const QString& arg : app.arguments()) {
        if (!arg.startsWith(QStringLiteral("--renderer-selftest"))) continue;
        const qsizetype eq = arg.indexOf(QLatin1Char('='));
        return runRendererSelfTest(eq < 0 ? QString() : arg.mid(eq + 1));
    }

    QIcon appIcon;
    appIcon.addFile(QStringLiteral(":/icons/trace-16.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-32.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-48.png"));
    appIcon.addFile(QStringLiteral(":/icons/trace-256.png"));
    app.setWindowIcon(appIcon);

    QPalette p = app.palette();
    p.setColor(QPalette::Window, QColor(20, 20, 20));
    p.setColor(QPalette::WindowText, QColor(230, 230, 230));
    p.setColor(QPalette::Base, QColor(10, 10, 10));
    p.setColor(QPalette::Text, QColor(230, 230, 230));
    p.setColor(QPalette::Button, QColor(35, 35, 35));
    p.setColor(QPalette::ButtonText, QColor(230, 230, 230));
    app.setPalette(p);

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
