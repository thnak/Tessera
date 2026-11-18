// SPDX-License-Identifier: MIT

// The browser entry point of the diagnostics module (issue #165, P1 of the design-time preview).
//
// The module diagnoses a PROJECT: the page stages the project's files with the four shared
// `tessera_project_*` exports (`wasm_project_exports.cpp`) and then calls
//   const char* tessera_diagnostics_json()
//       Runs the same stages and the same serializer as `tessera_codegen --diagnostics-json` over
//       the staged project -- the entry and every file it imports -- and returns a NUL-terminated
//       UTF-8 JSON document. Each diagnostic names the file its span is measured in.
//   void tessera_free(const char* text)
//       Releases a document returned above. Call it (or `tessera_diagnostics_free`) after each Run.
//
// The returned pointer is owned by the module: it is a freshly allocated block, valid until
// `tessera_free` is called on it, so a page that frees every result leaks nothing and a page that
// forgets one leaks one block per Run. This is the "matching free" convention rather than a static
// buffer: a static buffer would be invalidated by the next call, which makes a page that renders
// asynchronously read freed-looking memory, and it would force a copy anyway.
//
// Nothing here reads the filesystem, so this module must NOT be linked with NODERAWFS: see
// docs/design/compiler-wasm.md section 2.1 for why that option is meaningless and harmful in a
// browser build, and why it stays on the Node-only parity tool instead.

#include "diagnose.hpp"
#include "wasm_project_stage.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

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

EMSCRIPTEN_KEEPALIVE const char* tessera_diagnostics_json() {
    return duplicate_out(tessera::compiler::codegen::cli::diagnose_project(
        tessera::compiler::codegen::cli::project_stage().input()));
}

// The matching release. Named tessera_free so a page reads "the module owns what it returns and I
// hand it back" from the call site alone.
EMSCRIPTEN_KEEPALIVE void tessera_free(const char* text) {
    std::free(const_cast<char*>(text));
}

} // extern "C"
