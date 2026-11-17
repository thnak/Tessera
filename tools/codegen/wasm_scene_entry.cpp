// The browser entry point of the scene preview module (issue #166, P2, issue #168, P4, and issue
// #169, P5, of docs/design/reference-equantic-ui.md section 7.4).
//
// Exports, one convention, matching tools/codegen/wasm_entry.cpp (P1) exactly so a page loads every
// module with the same glue:
//   const char* tessera_scene_json(const char* source, const char* file, const char* component)
//       Evaluates `source` as the single file `file`, builds the component's scene, lays it out on
//       the null RHI (no GPU, no device) at the preview's default 320x240 at DPR 1, and returns the
//       canonical UiSnapshot document of docs/specs/automation-ids-v1.md section 5.2. This is the
//       signature scripts/ci/scene_snapshot_parity.py drives, so its behaviour must stay
//       byte-identical to the native CLI's defaults.
//   const char* tessera_scene_json_sized(const char* source, const char* file,
//                                        const char* component, int width, int height, float dpr)
//       The same document for an explicit viewport and DPR (issue #169: the P5 toolbar's viewport
//       and DPR toggles). A non-positive argument falls back to the default rather than laying out
//       a degenerate tree.
//   const char* tessera_scene_json_folded(const char* source, const char* file,
//                                         const char* component, const char* fold_posture,
//                                         int width, int height, float dpr)
//       The same document for a simulated foldable (issue #408): `fold_posture` is the studio's
//       posture name (`flat`, `book`, `tabletop`; empty keeps the unfolded device). An unknown name
//       is reported by the document's own error, because a silently ignored posture would make the
//       studio look like it worked while previewing an unfolded screen. Pointers first, numbers
//       after, matching `tessera_scene_json_sized` and the page's `callExport` marshalling.
//   const char* tessera_input_json(const char* source, const char* file, const char* component,
//                                  const char* script)
//       The same evaluation with an input script applied through the engine's `ui::InputRouter`:
//       one line per step, `click <automationId>`. Returns the state cells after every step (see
//       tools/codegen/scene_interaction.hpp for the document). This is the export a page calls for
//       issue #168.
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

#include <cstdlib>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

// The preview's default viewport, named because the default export and the native CLI's defaults
// must agree on it or the parity gate compares two different rectangles.
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

// The ONE options builder, so the default export, the sized export and the input export cannot
// drift in what they hand the driver. `salt` is the project namespace the ids are hashed with; the
// page has one project, so it is a constant until a later stage makes it adjustable, and that stage
// passes it through this surface rather than adding a fourth export.
tessera::compiler::codegen::cli::ScenePreviewOptions
preview_options(const char* file, float width, float height, float dpi_scale,
                tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None,
                const char* stylesheet = nullptr) {
    tessera::compiler::codegen::cli::ScenePreviewOptions options;
    if (file != nullptr && file[0] != '\0') {
        options.file = file;
    }
    options.width = width;
    options.height = height;
    options.dpi_scale = dpi_scale;
    options.salt = "preview";
    options.posture = posture;
    // Null or empty is "no author CSS" (issue #390): the preview keeps the built-in fallback, which
    // is what the plain P2 export, the parity gates and an older page all rely on.
    options.stylesheet = stylesheet != nullptr ? std::string_view(stylesheet) : std::string_view();
    return options;
}

std::string_view name_or_empty(const char* name) {
    return name != nullptr ? std::string_view(name) : std::string_view();
}

// The posture a page named, or `None` when it named none. A name the HAL does not know is NOT
// silently treated as "unfolded": the caller reports it (see `folded_document`), because a studio
// control that appears to work while previewing an unfolded screen is the one failure mode this
// slice must not have.
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

char* document_for(const char* source, const char* file, const char* component, float width,
                   float height, float dpi_scale,
                   tessera::hal::FoldPosture posture = tessera::hal::FoldPosture::None,
                   const char* stylesheet = nullptr) {
    if (source == nullptr) {
        // A page that has not run yet asks for nothing; answer with an empty document rather than a
        // null pointer, so the caller has one shape to handle. The gate's minimum-node guard is
        // what keeps "both sides empty" from counting as parity.
        return duplicate_out("{\"snapshotVersion\":1,\"sequence\":\"0\",\"nodes\":[]}\n");
    }
    return duplicate_out(tessera::compiler::codegen::cli::scene_preview_document(
        source, preview_options(file, width, height, dpi_scale, posture, stylesheet),
        name_or_empty(component)));
}

// The empty document the size/fold exports answer with when an argument names no posture. It is the
// same shape as a failed run, with the reason in the document the page already parses.
char* unknown_posture_document(const std::string_view posture) {
    std::string document = "{\"snapshotVersion\":1,\"sequence\":\"0\",\"error\":";
    document += "\"TESSERA_STUDIO_FOLD_POSTURE: `";
    document.append(posture);
    document += "` is not a fold posture; expected flat, book or tabletop\",\"nodes\":[]}\n";
    return duplicate_out(document);
}

// The input-script document for `source` over the stylesheet `stylesheet`, shared by the two input
// exports so the unstyled one cannot drift from the styled one (issue #390).
char* input_document_for(const char* source, const char* file, const char* component,
                         const char* script, const char* stylesheet) {
    if (source == nullptr) {
        return duplicate_out("{\"inputVersion\":1,\"error\":\"no source\",\"steps\":[]}\n");
    }
    return duplicate_out(tessera::compiler::codegen::cli::scene_input_document(
        source,
        preview_options(file, kDefaultWidth, kDefaultHeight, kDefaultDpiScale,
                        tessera::hal::FoldPosture::None, stylesheet),
        name_or_empty(component),
        script != nullptr ? std::string_view(script) : std::string_view()));
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* tessera_scene_json(const char* source, const char* file,
                                                    const char* component) {
    // No stylesheet: the pre-#390 signature, kept byte-identical so the P2 parity gate's default
    // and every caller written before author CSS existed keep the built-in fallback.
    return document_for(source, file, component, kDefaultWidth, kDefaultHeight, kDefaultDpiScale);
}

// The P2 scene document with the author's stylesheet (issue #390). `stylesheet` is appended after
// the existing arguments so a page built against the previous module still calls the export above;
// null or empty means "no author CSS" and produces the same bytes `tessera_scene_json` does.
EMSCRIPTEN_KEEPALIVE const char* tessera_scene_json_styled(const char* source, const char* file,
                                                           const char* component,
                                                           const char* stylesheet) {
    return document_for(source, file, component, kDefaultWidth, kDefaultHeight, kDefaultDpiScale,
                        tessera::hal::FoldPosture::None, stylesheet);
}

EMSCRIPTEN_KEEPALIVE const char* tessera_scene_json_sized(const char* source, const char* file,
                                                          const char* component, int width,
                                                          int height, float dpr) {
    // A page cannot conjure a degenerate viewport through a share link, but a caller can still pass
    // zero: fall back to the defaults instead of laying out a zero-sized tree and reporting it as a
    // successful empty document.
    const float w = width > 0 ? static_cast<float>(width) : kDefaultWidth;
    const float h = height > 0 ? static_cast<float>(height) : kDefaultHeight;
    const float scale = dpr > 0.0f ? dpr : kDefaultDpiScale;
    return document_for(source, file, component, w, h, scale);
}

// The styled, sized form: the Scene view's export once a stylesheet exists (issue #390). The
// unstyled sized and folded exports are kept because a page published against an older module calls
// them, and because the parity gates pin their bytes.
EMSCRIPTEN_KEEPALIVE const char*
tessera_scene_json_styled_sized(const char* source, const char* file, const char* component,
                                const char* stylesheet, int width, int height, float dpr) {
    const float w = width > 0 ? static_cast<float>(width) : kDefaultWidth;
    const float h = height > 0 ? static_cast<float>(height) : kDefaultHeight;
    const float scale = dpr > 0.0f ? dpr : kDefaultDpiScale;
    return document_for(source, file, component, w, h, scale, tessera::hal::FoldPosture::None,
                        stylesheet);
}

EMSCRIPTEN_KEEPALIVE const char* tessera_scene_json_folded(const char* source, const char* file,
                                                           const char* component,
                                                           const char* fold_posture, int width,
                                                           int height, float dpr) {
    // The pointer arguments come first and the numbers after, which is the shape every export here
    // uses: the page's `callExport` marshals the strings into the module heap and then passes the
    // resulting pointers followed by the numeric arguments, so a pointer after a number would be
    // handed a stale integer instead of a marshalled string.
    bool unknown = false;
    const tessera::hal::FoldPosture posture = posture_or_none(fold_posture, unknown);
    if (unknown) {
        return unknown_posture_document(name_or_empty(fold_posture));
    }
    const float w = width > 0 ? static_cast<float>(width) : kDefaultWidth;
    const float h = height > 0 ? static_cast<float>(height) : kDefaultHeight;
    const float scale = dpr > 0.0f ? dpr : kDefaultDpiScale;
    return document_for(source, file, component, w, h, scale, posture);
}

// The folded AND styled form: a fold posture and an author stylesheet are independent inputs, so
// the page would otherwise have to choose between them (issue #390). `stylesheet` is last, matching
// `tessera_scene_json_folded`'s pointer-then-number marshalling with one more pointer in front.
EMSCRIPTEN_KEEPALIVE const char*
tessera_scene_json_folded_styled(const char* source, const char* file, const char* component,
                                 const char* fold_posture, const char* stylesheet, int width,
                                 int height, float dpr) {
    bool unknown = false;
    const tessera::hal::FoldPosture posture = posture_or_none(fold_posture, unknown);
    if (unknown) {
        return unknown_posture_document(name_or_empty(fold_posture));
    }
    const float w = width > 0 ? static_cast<float>(width) : kDefaultWidth;
    const float h = height > 0 ? static_cast<float>(height) : kDefaultHeight;
    const float scale = dpr > 0.0f ? dpr : kDefaultDpiScale;
    return document_for(source, file, component, w, h, scale, posture, stylesheet);
}

EMSCRIPTEN_KEEPALIVE const char* tessera_input_json(const char* source, const char* file,
                                                    const char* component, const char* script) {
    // The pre-#390 signature, unchanged: the input parity gate pins it, and an older page calls it.
    // No author CSS, so the built-in fallback applies exactly as it did before this issue.
    return input_document_for(source, file, component, script, nullptr);
}

// The styled form (issue #390): a scripted click resolves style against the same sheet the page
// shows, so its state cells cannot come from a differently styled tree. Null/empty keeps the
// fallback, which makes this byte-identical to `tessera_input_json` for a caller with no CSS.
EMSCRIPTEN_KEEPALIVE const char* tessera_input_json_styled(const char* source, const char* file,
                                                           const char* component,
                                                           const char* script,
                                                           const char* stylesheet) {
    return input_document_for(source, file, component, script, stylesheet);
}

// The matching release, named as in the diagnostics module so a page reads "the module owns what it
// returns and I hand it back" from the call site alone.
EMSCRIPTEN_KEEPALIVE void tessera_free(const char* text) {
    std::free(const_cast<char*>(text));
}

// The author stylesheet's parse problems (issue #390), as one JSON document on one line:
//
//   {"cssVersion":1,"styled":true,"truncated":false,"file":"ui/Preview.css","diagnostics":[...]}
//
// `styled:false` means "no author CSS", so the page can say the built-in sheet is in force instead
// of showing an empty list that reads as "the sheet is fine". Null or empty `stylesheet` always
// answers `styled:false`, so a page built before this issue still gets a well-formed document.
//
// **Why a separate export rather than a field of the snapshot.** The snapshot document is the P2
// parity artifact and `scene_snapshot_parity.py` pins its bytes; a stylesheet problem is not part
// of the tree. The diagnostics module cannot carry it either: it links no stylesheet engine by
// design, so the module that already compiled the sheet is the one that reports it. The writer is
// `css_diagnostics_report.cpp`.
EMSCRIPTEN_KEEPALIVE const char* tessera_css_diagnostics_json(const char* stylesheet,
                                                              const char* stylesheet_file) {
    return duplicate_out(tessera::compiler::codegen::cli::css_diagnostics_json(
        stylesheet != nullptr ? std::string_view(stylesheet) : std::string_view(),
        stylesheet_file != nullptr ? std::string_view(stylesheet_file) : std::string_view()));
}

} // extern "C"
