// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/ir/module.hpp"

#include <expected>
#include <string>
#include <vector>

// First C++ codegen slice (docs/design/codegen.md): one `.hpp` and one `.cpp` per IR component.
// The generated translation unit is a pure function of the IR: same IR, same bytes.
namespace tessera::compiler::codegen {

struct GeneratedFile {
    std::string path; // deterministic, `<Component>.hpp` / `<Component>.cpp`
    std::string text;
};

// One `TESSERA_CODEGEN_*` error. Lowering/codegen stops at the first unsupported construct; no
// partial file set is returned.
using CodegenErrors = std::vector<std::string>;
using CodegenResult = std::expected<std::vector<GeneratedFile>, CodegenErrors>;

// Generates the C++ files of every component of `module`, sorted by component name. The module name
// becomes the C++ namespace root of the generated structs.
[[nodiscard]] CodegenResult generate(const ir::IrModule& module);

// Non-fatal findings for `module`, one `TESSERA_CG_*: ...` string each, in a deterministic order.
// Today: every stylesheet the IR lists is `TESSERA_CG_STYLESHEET_NOT_EMBEDDED`, because generated
// code does not yet deliver CSS to the app (issue #80; embedding is issue #81).
[[nodiscard]] std::vector<std::string> codegen_warnings(const ir::IrModule& module);

} // namespace tessera::compiler::codegen
