// SPDX-License-Identifier: MIT
//
// The exports every preview module shares for handing it a project (see
// `wasm_project_stage.hpp`). They live in one translation unit so the diagnostics, scene and canvas
// modules cannot stage a project three slightly different ways.
//
// A null path or text is an empty string, not a crash: the caller is JavaScript, and a missing
// argument must cost an empty file, never an abort of the module (which would lose the page's
// unsaved work).

#include "wasm_project_stage.hpp"

#include <string_view>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

std::string_view view_of(const char* text) {
    return text != nullptr ? std::string_view(text) : std::string_view();
}

} // namespace

extern "C" {

// Forgets the staged project. Call it before staging a different one.
EMSCRIPTEN_KEEPALIVE void tessera_project_begin() {
    tessera::compiler::codegen::cli::project_stage().begin();
}

// Names the file whose component the preview shows (`app.entry` of tessera.json). A project with no
// entry previews nothing.
EMSCRIPTEN_KEEPALIVE void tessera_project_set_entry(const char* path) {
    tessera::compiler::codegen::cli::project_stage().set_entry(view_of(path));
}

// Stages one `.tsx` / `.jsx` file at its canonical project-relative POSIX path.
EMSCRIPTEN_KEEPALIVE void tessera_project_add_source(const char* path, const char* text) {
    tessera::compiler::codegen::cli::project_stage().add_source(view_of(path), view_of(text));
}

// Stages one `.css` file. It applies only if a staged source imports it.
EMSCRIPTEN_KEEPALIVE void tessera_project_add_stylesheet(const char* path, const char* text) {
    tessera::compiler::codegen::cli::project_stage().add_stylesheet(view_of(path), view_of(text));
}

// Stages the project's `ui/services.json` text (docs/specs/services-manifest-v1.md). Without it a
// `useService<T>()` in a staged source is rejected, exactly as in a project with no manifest.
EMSCRIPTEN_KEEPALIVE void tessera_project_set_services(const char* text) {
    tessera::compiler::codegen::cli::project_stage().set_services_manifest(view_of(text));
}

} // extern "C"
