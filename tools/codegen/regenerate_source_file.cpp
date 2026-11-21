// SPDX-License-Identifier: MIT

#include "regenerate_source_file.hpp"

#include <utility>

namespace tessera::compiler::codegen::cli::detail {

SourceFile::SourceFile(std::string file_path, std::string file_text, const jsx::Limits& limits,
                       const jsx::FrontEndOptions& options)
    : path(std::move(file_path)),
      text(std::move(file_text)),
      parsed(jsx::parse_module(text, limits, options)),
      map(text) {
    if (parsed.root == nullptr) {
        return;
    }
    // The custom-hook expansion runs BETWEEN the parse and the analysis, which is the order the
    // spec requires (docs/specs/custom-hooks-v1.md section 4): the rules must see the expanded
    // component, not the hook call. With the switch off this is a no-op, so a project that uses no
    // custom hook is byte-identical to before.
    //
    // The sink is kept on the file rather than discarded because the expansion's own diagnostics
    // are part of the run's report, and because the call-site map it fills is what lets a later
    // diagnostic name the call site (section 8).
    expansion_sink = std::make_unique<jsx::DiagnosticSink>(map, limits);
    expansion = jsx::expand_custom_hooks(*parsed.root, map, options, *expansion_sink);
    analyzed = rules::analyze_module(*parsed.root, map);
}

bool SourceFile::parses() const {
    return parsed.root != nullptr && !parsed.has_errors();
}

lowering::ModuleSource SourceFile::module_source() const {
    return lowering::ModuleSource{path, parsed.root.get(), &analyzed, &map};
}

} // namespace tessera::compiler::codegen::cli::detail
