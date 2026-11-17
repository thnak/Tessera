// SPDX-License-Identifier: MIT

#include "regenerate_source_file.hpp"

#include <utility>

namespace tessera::compiler::codegen::cli::detail {

SourceFile::SourceFile(std::string file_path, std::string file_text)
    : path(std::move(file_path)),
      text(std::move(file_text)),
      parsed(jsx::parse_module(text)),
      map(text),
      analyzed(parsed.root != nullptr ? rules::analyze_module(*parsed.root, map)
                                      : rules::AnalyzedModule{}) {}

bool SourceFile::parses() const {
    return parsed.root != nullptr && !parsed.has_errors();
}

lowering::ModuleSource SourceFile::module_source() const {
    return lowering::ModuleSource{path, parsed.root.get(), &analyzed, &map};
}

} // namespace tessera::compiler::codegen::cli::detail
