// SPDX-License-Identifier: MIT

// The browser entry point of the scene preview module (issue #166, P2 of the design-time preview).
//
// Two exports, one convention, matching tools/codegen/wasm_entry.cpp (P1) exactly, so a page loads
// both modules with the same glue:
//   const char* tessera_scene_json(const char* source, const char* file, const char* component)
//       Evaluates `source` as the single file `file`, builds the component's scene, lays it out on
//       the null RHI (no GPU, no device) and returns the canonical UiSnapshot document of
//       docs/specs/automation-ids-v1.md section 5.2 as a NUL-terminated UTF-8 JSON string.
//   void tessera_free(const char* text)
//       Releases a document returned above.
//
// The returned pointer is owned by the module: a freshly allocated block, valid until
// `tessera_free` is called on it, so a page that frees every result leaks nothing and a page that
// forgets one leaks one block per Run. That is the "matching free" convention rather than a static
// buffer, because a static buffer would be invalidated by the next call (a page that renders
// asynchronously would read freed-looking memory) and would force a copy anyway.
//
// Nothing here reads the filesystem, so this module must NOT be linked with NODERAWFS: see
// docs/design/compiler-wasm.md section 2.1 for why that option is meaningless and harmful in a
// browser build, and why it stays on the Node-only parity tool instead.

#include "scene_preview.hpp"

#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

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

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* tessera_scene_json(const char* source, const char* file,
                                                    const char* component) {
    if (source == nullptr) {
        // A page that has not run yet asks for nothing; answer with the empty document rather than
        // a null pointer, so the caller has one shape to handle. The gate's minimum-node guard is
        // what keeps "both sides empty" from counting as parity.
        return duplicate_out("{\"snapshotVersion\":1,\"sequence\":\"0\",\"nodes\":[]}\n");
    }
    tessera::compiler::codegen::cli::ScenePreviewOptions options;
    if (file != nullptr && file[0] != '\0') {
        options.file = file;
    }
    // The viewport and salt of the preview page. They are constants here rather than parameters
    // because the page has one viewport and one project namespace in this slice; a later stage that
    // makes them adjustable (P5's share links, P6's budgets) passes them through the same export
    // rather than adding a second one.
    options.width = 320.0f;
    options.height = 240.0f;
    options.dpi_scale = 1.0f;
    options.salt = "preview";
    const std::string_view name =
        component != nullptr ? std::string_view(component) : std::string_view();
    return duplicate_out(
        tessera::compiler::codegen::cli::scene_preview_document(source, options, name));
}

// The matching release, named as in the diagnostics module so a page reads "the module owns what it
// returns and I hand it back" from the call site alone.
EMSCRIPTEN_KEEPALIVE void tessera_free(const char* text) {
    std::free(const_cast<char*>(text));
}

} // extern "C"
