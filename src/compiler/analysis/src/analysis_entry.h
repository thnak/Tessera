// SPDX-License-Identifier: MIT

#pragma once

// The C ABI of the analysis worker (ADR-060, docs/specs/studio-services-v1.md sections 8 and 10).
// The Wasm module exports these, and the native parity test links the same functions, so what a
// page receives is what the library returns. One project per module instance; the worker owns the
// snapshot and the version, the module only keeps the index.
//
//   snapshot: tessera_analysis_set_file for every file, then tessera_analysis_commit_snapshot
//   delta:    tessera_analysis_edit for every edit in order, then tessera_analysis_commit_edits
//   query:    tessera_analysis_query_json("symbols" | "diagnostics" | "capabilities", file or -1)
//
// Strings are NUL-terminated UTF-8. A returned string is released with tessera_free; null is an
// error the worker reports (out of memory, unknown query type), never a crash.

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

extern "C" {

EMSCRIPTEN_KEEPALIVE void tessera_analysis_reset();
EMSCRIPTEN_KEEPALIVE void tessera_analysis_set_file(int file, const char* path, const char* text);
EMSCRIPTEN_KEEPALIVE void tessera_analysis_commit_snapshot();
EMSCRIPTEN_KEEPALIVE void tessera_analysis_edit(int file, int offset, int length,
                                                const char* new_text);
// 1 when every staged edit applied, 0 when one named an unknown file or a range outside it (the
// index is then unchanged and the worker answers TESSERA_STUDIO_VERSION so the page resends a
// snapshot).
EMSCRIPTEN_KEEPALIVE int tessera_analysis_commit_edits();
EMSCRIPTEN_KEEPALIVE const char* tessera_analysis_query_json(const char* type, int file);
EMSCRIPTEN_KEEPALIVE void tessera_free(const char* text);
}
