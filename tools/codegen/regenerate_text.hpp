// SPDX-License-Identifier: MIT

// Pure text helpers of the codegen CLI driver.
//
// Separated from `regenerate.cpp` because that file is over the 500-line hard limit with a recorded
// 668-line baseline, and these four are the part of it with **no dependency on the driver's
// state**: they format a diagnostic, vet an output file name, or derive a namespace from a path.
// Declaring them here exposes nothing of `Outcome`, `Options` or the report sink -- which is why
// this seam is safe where extracting the developer-file handling was not.

#pragma once

#include "tessera/compiler/ir/validate.hpp"
#include "tessera/compiler/jsx/diagnostic.hpp"

#include <string>
#include <string_view>

namespace tessera::compiler::codegen::cli::detail {

// `path:line:column: severity: message` for a front-end diagnostic.
std::string diagnostic_text(std::string_view path, const jsx::Diagnostic& diagnostic);

// The same shape for an IR diagnostic, using the code the validator assigned.
std::string ir_diagnostic_text(const ir::IrDiagnostic& diagnostic);

// False when `name` could escape the output directory (separators, `..`, empty).
bool safe_file_name(std::string_view name);

// The `--namespace` fallback: the file stem with non-identifier characters replaced, a leading
// digit prefixed, lowercased (C++ namespace convention).
std::string derive_module_name(const std::string& input);

} // namespace tessera::compiler::codegen::cli::detail
