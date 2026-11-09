// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Internal seam of the codegen slice. Not installed; only src/compiler/codegen includes it.
namespace tessera::compiler::codegen::internal {

// Deterministic line-oriented C++ writer: two-space indent, LF only, one trailing newline. It never
// runs clang-format, so output depends only on the IR (docs/design/codegen.md).
struct CppWriter {
    std::string text;
    int indent = 0;

    void line(std::string_view text);
    void open(std::string_view text);         // line + "{"
    void close(std::string_view suffix = {}); // "}" or "};"
};

// A C++ type for an IR type (docs/specs/ir-v1.md section 2). Null when the type needs a runtime
// type this slice does not have; the caller reports TESSERA_CODEGEN_UNSUPPORTED.
[[nodiscard]] std::optional<std::string> cpp_type(std::string_view ir_type);

// C++ literal text for one IR literal (string literals are escaped).
[[nodiscard]] std::string literal_text(const ir::LiteralValue& literal);

// The generated member name for a reference id (`state::count` -> `state_count_`).
[[nodiscard]] std::string member_name(std::string_view ir_id);

// Emits one IR expression as C++ source. A nullopt `text` carries the error message in `error`.
struct ExprEmission {
    std::string text;
    std::string error;
};
[[nodiscard]] ExprEmission emit_expr(const ir::Expr& expr, const ir::Component& component);

// Emits the body of an inline handler or preview: one line per action.
[[nodiscard]] std::optional<std::string> emit_actions(const std::vector<ir::Action>& actions,
                                                      const ir::Component& component,
                                                      std::string& error);

// Appends `<Component>.hpp` and `<Component>.cpp` to `files`, or sets `error` and returns false.
[[nodiscard]] bool emit_component(const ir::Component& component, std::string_view module_namespace,
                                  const std::vector<ir::Binding>& bindings,
                                  std::vector<GeneratedFile>& files, std::string& error);

} // namespace tessera::compiler::codegen::internal
