// SPDX-License-Identifier: MIT

// The browser entry point of the format service (ADR-061 slice 2, ADR-060): the studio's format
// worker loads this module and calls
//   const char* tessera_format_edits_json(const char* text, int print_width, int indent_width)
//       the minimal edit list of `format_edits_json` for one .tsx file: UTF-8 JSON
//       {"edits":[{offset,length,newText}],"diagnostics":[...]}, offsets in UTF-16 code units.
//   void tessera_free(const char* text)
//       releases a document returned above (the same matching-free convention as the other preview
//       modules, see designer/web/src/lib/wasm-call.ts).
// Nothing here reads a file, so the module is not linked with NODERAWFS.

#include "tessera/compiler/format/edits.hpp"

#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__EMSCRIPTEN__)
#include <emscripten/emscripten.h>
#else
#define EMSCRIPTEN_KEEPALIVE
#endif

namespace {

// The C ABI boundary: the caller is JavaScript, which cannot receive a std::string or free one. A
// null return is an error the page reports, not a crash.
char* duplicate_out(const std::string& text) {
    char* out = static_cast<char*>(std::malloc(text.size() + 1));
    if (out == nullptr) {
        return nullptr;
    }
    std::memcpy(out, text.c_str(), text.size() + 1);
    return out;
}

} // namespace

extern "C" {

EMSCRIPTEN_KEEPALIVE const char* tessera_format_edits_json(const char* text, int print_width,
                                                           int indent_width) {
    namespace format = tessera::compiler::format;
    format::FormatOptions options;
    if (print_width > 0) {
        options.print_width = print_width;
    }
    if (indent_width > 0) {
        options.indent_width = indent_width;
    }
    return duplicate_out(
        format::format_edits_json(format::FileKind::Tsx, text == nullptr ? "" : text, options));
}

EMSCRIPTEN_KEEPALIVE void tessera_free(const char* text) {
    std::free(const_cast<char*>(text));
}

} // extern "C"
