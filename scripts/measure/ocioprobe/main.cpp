// ocioprobe -- what the OpenColorIO API ACTUALLY does on this build, measured
// rather than assumed, before the Color Transform dialog is designed around it.
//
// It exists for the same reason exrprobe does: the dialog's config discovery has
// to be written against real API behaviour, and three of the questions below
// have plausible wrong answers that would only show up as a dialog that is empty
// or that silently loads the wrong thing.
//
//   1. Does Config::CreateFromEnv() throw when $OCIO is unset, or fall back?
//      The answer decides whether "the $OCIO config" can be a menu entry that is
//      simply always present, or has to be gated on the variable being set.
//   2. What built-in configs does this OCIO carry, and what are their exact
//      names? The dialog offers them so Trace works with nothing installed.
//   3. For a real config: what does the scene_linear ROLE resolve to, and what
//      does getColorSpaceFromFilepath() answer for a .exr? Stage 0 measured
//      ACEScg against Raw on the Redshift config -- the wrong-but-plausible
//      answer that looks correct in every API call and flat on screen. Re-run
//      here so the dialog's default is chosen against a measurement.
//   4. What displays and views does it offer, spelled exactly?
//
// Build:
//   cmake -S scripts/measure/ocioprobe -B build-ocioprobe -A x64 \
//         -DCMAKE_TOOLCHAIN_FILE=C:/vcpkg/scripts/buildsystems/vcpkg.cmake
//   cmake --build build-ocioprobe --config Release

#include <OpenColorIO/OpenColorIO.h>

#include <cstdio>
#include <cstdlib>
#include <exception>
#include <string>

namespace OCIO = OCIO_NAMESPACE;

static void line() { std::printf("--------------------------------------------------\n"); }

static void probeEnv() {
    line();
    const char* env = std::getenv("OCIO");
    std::printf("$OCIO = %s\n", (env && *env) ? env : "(unset)");
    try {
        auto cfg = OCIO::Config::CreateFromEnv();
        if (!cfg) {
            std::printf("CreateFromEnv() -> NULL (no throw)\n");
            return;
        }
        std::printf("CreateFromEnv() -> ok, name='%s' description='%.60s'\n",
                    cfg->getName() ? cfg->getName() : "",
                    cfg->getDescription() ? cfg->getDescription() : "");
        std::printf("                   numColorSpaces=%d numDisplays=%d\n",
                    cfg->getNumColorSpaces(), cfg->getNumDisplays());
    } catch (const std::exception& e) {
        std::printf("CreateFromEnv() -> THREW: %s\n", e.what());
    }
}

static void probeBuiltins() {
    line();
    try {
        const auto& reg = OCIO::BuiltinConfigRegistry::Get();
        const std::size_t n = reg.getNumBuiltinConfigs();
        std::printf("builtin configs: %zu\n", n);
        for (std::size_t i = 0; i < n; ++i) {
            std::printf("  [%zu] name='%s'\n       ui='%s' recommended=%d\n",
                        i,
                        reg.getBuiltinConfigName(i) ? reg.getBuiltinConfigName(i) : "",
                        reg.getBuiltinConfigUIName(i) ? reg.getBuiltinConfigUIName(i) : "",
                        reg.isBuiltinConfigRecommended(i) ? 1 : 0);
        }
        // MEASURED: there is no getDefaultBuiltinConfigName() on this OCIO.
        // "ocio://default" is resolved by the Config factory, not by the
        // registry -- so the dialog cannot ask the registry which one is the
        // default and must carry the URI instead.
    } catch (const std::exception& e) {
        std::printf("builtin registry THREW: %s\n", e.what());
    }
}

static void describe(const char* label, OCIO::ConstConfigRcPtr cfg) {
    if (!cfg) { std::printf("%s -> NULL\n", label); return; }
    std::printf("%s\n", label);
    const char* sl = cfg->getCanonicalName(OCIO::ROLE_SCENE_LINEAR);
    std::printf("  scene_linear role            = '%s'\n", (sl && *sl) ? sl : "(none)");
    try {
        const char* fr = cfg->getColorSpaceFromFilepath("foo.exr");
        std::printf("  getColorSpaceFromFilepath(.exr) = '%s'   <-- NEVER use this as the default\n",
                    (fr && *fr) ? fr : "(none)");
    } catch (const std::exception& e) {
        std::printf("  getColorSpaceFromFilepath(.exr) THREW: %s\n", e.what());
    }
    std::printf("  colour spaces = %d\n", cfg->getNumColorSpaces());
    const int nd = cfg->getNumDisplays();
    std::printf("  displays = %d   (default '%s')\n", nd,
                cfg->getDefaultDisplay() ? cfg->getDefaultDisplay() : "");
    for (int d = 0; d < nd; ++d) {
        const char* dn = cfg->getDisplay(d);
        std::printf("    display[%d] '%s'  (default view '%s')\n", d, dn,
                    cfg->getDefaultView(dn) ? cfg->getDefaultView(dn) : "");
        const int nv = cfg->getNumViews(dn);
        for (int v = 0; v < nv; ++v) {
            std::printf("        view[%d] '%s'\n", v, cfg->getView(dn, v));
        }
    }
}

int main(int argc, char** argv) {
    std::printf("OpenColorIO %s\n", OCIO::GetVersion());
    probeEnv();
    probeBuiltins();

    line();
    try {
        describe("ocio://default", OCIO::Config::CreateFromBuiltinConfig("ocio://default"));
    } catch (const std::exception& e) {
        std::printf("ocio://default THREW: %s\n", e.what());
    }

    // CreateFromFile with a builtin URI -- does the same entry point take both?
    // If it does, the dialog needs ONE code path for "a config" rather than a
    // branch on whether the string looks like a URI.
    line();
    try {
        auto cfg = OCIO::Config::CreateFromFile("ocio://default");
        std::printf("CreateFromFile(\"ocio://default\") -> %s\n", cfg ? "ok" : "NULL");
    } catch (const std::exception& e) {
        std::printf("CreateFromFile(\"ocio://default\") THREW: %s\n", e.what());
    }

    for (int i = 1; i < argc; ++i) {
        line();
        try {
            describe(argv[i], OCIO::Config::CreateFromFile(argv[i]));
        } catch (const std::exception& e) {
            std::printf("%s THREW: %s\n", argv[i], e.what());
        }
    }

    // A path that does not exist, so the dialog's failure message is written
    // against the real exception text rather than an invented one.
    line();
    try {
        auto cfg = OCIO::Config::CreateFromFile("Z:/does/not/exist.ocio");
        std::printf("missing file -> %s\n", cfg ? "ok (!)" : "NULL");
    } catch (const std::exception& e) {
        std::printf("missing file THREW: %s\n", e.what());
    }
    return 0;
}
