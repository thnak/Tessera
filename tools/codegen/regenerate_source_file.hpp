// SPDX-License-Identifier: MIT

// One source file of a codegen run: its text, its parse, its source map and its analysis.
//
// Extracted from `regenerate.cpp` with the pure text helpers: the file is over the 500-line hard
// limit with a recorded 668-line baseline, and this struct is a data holder that depends on no part
// of the driver's state, so declaring it costs nothing.

#pragma once

#include "tessera/compiler/jsx/hook_expansion.hpp"
#include "tessera/compiler/jsx/parser.hpp"
#include "tessera/compiler/lowering/node_lowering.hpp"
#include "tessera/compiler/rules/analyze.hpp"
#include "tessera/compiler/services/manifest_types.hpp"

#include <memory>
#include <string>
#include <utility>

namespace tessera::compiler::codegen::cli::detail {

// One source file of a codegen run: its text, its parse, its source map and its analysis.
//
// Defined in `regenerate_source_file.cpp`: the constructor parses and analyses the text, so
// inlining it here would put the front end in every translation unit that reads a file.
struct SourceFile {
    SourceFile(std::string file_path, std::string file_text, const jsx::Limits& limits = {},
               const jsx::FrontEndOptions& options = {},
               const services::ServicesManifest* services = nullptr);

    SourceFile(const SourceFile&) = delete;
    SourceFile& operator=(const SourceFile&) = delete;

    [[nodiscard]] bool parses() const;
    [[nodiscard]] lowering::ModuleSource module_source() const;

    std::string path;
    std::string text;
    jsx::ModuleParseResult parsed;
    jsx::SourceMap map;
    // The custom-hook expansion's diagnostics and its call-site map (spike #659). Declared before
    // `analyzed` so it outlives the analysis, which borrows nothing from it but is reported after
    // it: a diagnostic the rules raise inside an expanded hook body needs the map to name the call
    // site (docs/specs/custom-hooks-v1.md section 8).
    std::unique_ptr<jsx::DiagnosticSink> expansion_sink;
    jsx::ExpansionMetrics expansion;
    rules::AnalyzedModule analyzed;
};

} // namespace tessera::compiler::codegen::cli::detail
