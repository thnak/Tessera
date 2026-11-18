// The browser entry point of the scene preview module (issue #166, P2, issue #168, P4, and issue
// #169, P5, of docs/design/reference-equantic-ui.md section 7.4).
//
// The module previews a PROJECT, not a buffer: the page stages the project's files with the four
// shared `tessera_project_*` exports (`wasm_project_exports.cpp`) and then calls
//   const char* tessera_scene_json(const char* component, const char* fold_posture,
//                                  int width, int height, float dpr)
//       Evaluates the staged project from its entry (the entry file's `component`, or its first
//       component when the name is empty), builds the scene, lays it out on the null RHI (no GPU,
//       no device) at the given viewport and DPR, and returns the canonical UiSnapshot document of
//       docs/specs/automation-ids-v1.md section 5.2. `fold_posture` is the studio's posture name
//       (`flat`, `book`, `tabletop`; empty keeps the unfolded device); an unknown name is reported
//       by the document's own error, because a silently ignored posture would make the studio look
//       like it worked while previewing an unfolded screen. A non-positive size or DPR falls back
//       to the preview's default (320x240 at 1) rather than laying out a degenerate tree. Pointers
//       first, numbers after, matching the page's `callExport` marshalling.
//   const char* tessera_input_json(const char* component, const char* script)
//       The same evaluation with an input script applied through the engine's `ui::InputRouter`:
//       one line per step, `click <automationId>`. Returns the state cells after every step (see
//       tools/codegen/scene_interaction.hpp for the document). This is the export a page calls for
//       issue #168.
//   const char* tessera_css_diagnostics_json()
//       The problems of the stylesheets the entry imports, one JSON document (see
//       `css_diagnostics_report.hpp`).
//
// There is no default entry and no default file name. A project with no entry answers with the
// document's own error; it is never previewed through some other file.
//
// The returned pointer is owned by the module: a freshly allocated block, valid until
// `tessera_free` is called on it, so a page that frees every result leaks nothing and a page that
// forgets one leaks one block per Run. That is the "matching free" convention rather than a static
// buffer, because a static buffer would be invalidated by the next call (a page that renders
// asynchronously would read freed-looking memory) and would force a copy anyway.
//
// **What runs on a click, and what cannot.** A click is dispatched by the engine's own
// `ui::InputRouter` (hit testing, pointer capture, click synthesis, capture/target/bubble), and the
// handler it reaches is the IR *preview body*, run by the evaluator. The developer's C++ is not
// linked into this module and is never executed: see tools/codegen/scene_session.hpp.
//
// Nothing here reads the filesystem, so this module must NOT be linked with NODERAWFS: see
// docs/design/compiler-wasm.md section 2.1 for why that option is meaningless and harmful in a
// browser build, and why it stays on the Node-only parity tool instead.

#include "css_diagnostics_report.hpp"
#include "scene_interaction.hpp"
#include "scene_preview.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/hal/fold_posture.hpp"
#include "wasm_project_stage.hpp"

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

namespace cli = tessera::compiler::codegen::cli;

// The preview's default viewport, named because the module and the native CLI's defaults must agree
// on it or the parity gate compares two different rectangles.
constexpr float kDefaultWidth = 320.0f;
constexpr float kDefaultHeight = 240.0f;
constexpr float kDefaultDpiScale = 1.0f;

// Copies `text` into a malloc'd block so the pointer stays valid across the module's next call. The
// allocation is the C ABI boundary this export needs: the caller is JavaScript, which cannot
// receive a C++ std::string and cannot free one. A failure returns null, which the page renders as
// an error rather than a crash.
char* duplicate_out(std::string text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

std::string_view name_or_empty(const char* name) {
    return name != nullptr ? std::string_view(name) : std::string_view();
}

// The ONE options builder, so the snapshot export and the input export cannot drift in what they
// hand the driver. `salt` is the project namespace the ids are hashed with; the page has one
// project, so it is a constant until a later stage makes it adjustable.
cli::ScenePreviewOptions preview_options(float width, float height, float dpi_scale,
                                         tessera::hal::FoldPosture posture) {
    cli::ScenePreviewOptions options;
    options.width = width > 0.0f ? width : kDefaultWidth;
    options.height = height > 0.0f ? height : kDefaultHeight;
    options.dpi_scale = dpi_scale > 0.0f ? dpi_scale : kDefaultDpiScale;
    options.salt = "preview";
    options.posture = posture;
    return options;
}

// The posture a page named, or `None` when it named none. A name the HAL does not know is NOT
// silently treated as "unfolded": the caller reports it, because a studio control that appears to
// work while previewing an unfolded screen is the one failure mode this must not have.
tessera::hal::FoldPosture posture_or_none(const char* name, bool& unknown) {
    unknown = false;
    const std::string_view text = name_or_empty(name);
    if (text.empty()) {
        return tessera::hal::FoldPosture::None;
    }
    const std::optional<tessera::hal::FoldPosture> parsed = tessera::hal::parse_fold_posture(text);
    if (!parsed) {
        unknown = true;
        return tessera::hal::FoldPosture::None;
    }
    return *parsed;
}

// An empty snapshot document that says why, in the one shape the page already parses.
char* failed_document(std::string_view reason) {
    std::string document = "{\"snapshotVersion\":1,\"sequence\":\"0\",\"error\":";
    document += cli::quoted(reason);
    document += ",\"nodes\":[]}\n";
    return duplicate_out(document);
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* tessera_scene_json(const char* component, const char* fold_posture,
                                                    int width, int height, float dpr) {
    const cli::ProjectStage& stage = cli::project_stage();
    if (!stage.has_entry()) {
        return failed_document("TESSERA_STUDIO_NO_ENTRY: the project names no entry file");
    }
    bool unknown = false;
    const tessera::hal::FoldPosture posture = posture_or_none(fold_posture, unknown);
    if (unknown) {
        return failed_document("TESSERA_STUDIO_FOLD_POSTURE: `" +
                               std::string(name_or_empty(fold_posture)) +
                               "` is not a fold posture; expected flat, book or tabletop");
    }
    return duplicate_out(cli::scene_preview_document(
        stage.input(),
        preview_options(static_cast<float>(width), static_cast<float>(height), dpr, posture),
        name_or_empty(component)));
}

EMSCRIPTEN_KEEPALIVE const char* tessera_input_json(const char* component, const char* script) {
    const cli::ProjectStage& stage = cli::project_stage();
    if (!stage.has_entry()) {
        return duplicate_out(
            "{\"inputVersion\":1,\"error\":\"the project names no entry file\",\"steps\":[]}\n");
    }
    return duplicate_out(
        cli::scene_input_document(stage.input(),
                                  preview_options(kDefaultWidth, kDefaultHeight, kDefaultDpiScale,
                                                  tessera::hal::FoldPosture::None),
                                  name_or_empty(component), name_or_empty(script)));
}

// The matching release, named as in the diagnostics module so a page reads "the module owns what it
// returns and I hand it back" from the call site alone.
EMSCRIPTEN_KEEPALIVE void tessera_free(const char* text) {
    std::free(const_cast<char*>(text));
}

// The imported stylesheets' parse problems (issue #390), as one JSON document on one line.
//
// **Why a separate export rather than a field of the snapshot.** The snapshot document is the P2
// parity artifact and `scene_snapshot_parity.py` pins its bytes; a stylesheet problem is not part
// of the tree. The diagnostics module cannot carry it either: it links no stylesheet engine by
// design, so the module that already compiled the sheet is the one that reports it. The writer is
// `css_diagnostics_report.cpp`.
EMSCRIPTEN_KEEPALIVE const char* tessera_css_diagnostics_json() {
    return duplicate_out(cli::css_diagnostics_json(cli::project_stage().input()));
}

} // extern "C"
