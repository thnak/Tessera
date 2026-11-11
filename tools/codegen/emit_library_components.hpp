// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <string>

namespace tessera::compiler::codegen::cli {

// Writes the C++ translation unit that rebuilds the presentational components of `module` as IR
// data (issue #70). Deterministic: the text is a pure function of the IR, with no timestamps and
// no absolute paths (the source file names must be project-relative). Returns false with `error`
// when a component cannot be expressed (non-presentational, malformed node id, unsupported
// expression) so the build fails instead of embedding a partial declaration.
[[nodiscard]] bool emit_library_components_cpp(const ir::IrModule& module, const std::string& path,
                                               std::string& error);

} // namespace tessera::compiler::codegen::cli
