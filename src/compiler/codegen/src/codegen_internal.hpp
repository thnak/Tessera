// SPDX-License-Identifier: MIT

#pragma once

#include "tessera/compiler/codegen/codegen.hpp"
#include "tessera/compiler/ir/action.hpp"
#include "tessera/compiler/ir/component.hpp"
#include "tessera/compiler/ir/expr.hpp"
#include "tessera/compiler/ir/module.hpp"
#include "tessera/compiler/ir/node.hpp"

#include <optional>
#include <span>
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

// One loop local (`item`, `index`) visible inside a generated list-item builder: the IR reference
// id the template expressions use, the C++ expression that names the value, and its IR element
// type. Generated item bodies bind the item by `const T&` and the index as `std::size_t`; the C++
// form is what expression emission substitutes for the IR reference.
struct LocalBinding {
    std::string name;
    std::string cpp;
    std::string ir_type;
};
using LocalBindings = std::vector<LocalBinding>;

// The binding for an IR reference id inside a list template, or nullptr outside one.
[[nodiscard]] const LocalBinding* find_local(const LocalBindings& locals, std::string_view id);

// Emits one IR expression as C++ source; `locals` supplies the loop item/index of a list template.
// A nullopt `text` carries the error message in `error`.
struct ExprEmission {
    std::string text;
    std::string error;
};
[[nodiscard]] ExprEmission emit_expr(const ir::Expr& expr, const ir::Component& component,
                                     const LocalBindings& locals = {});

// Emits the body of an inline handler or preview: one line per action.
[[nodiscard]] std::optional<std::string> emit_actions(const std::vector<ir::Action>& actions,
                                                      const ir::Component& component,
                                                      std::span<const ir::Param> params,
                                                      std::string& error);

// One interpolated text node: ordinal matches the generated node_N variables.
struct DynamicText {
    unsigned ordinal = 0;
    const ir::Expr* text = nullptr;
};

// An interpolated element metadata value (`id`, `className`, `role`, `aria-*`, `automationId`).
// The generated component owns the current string (node metadata stays a borrowed view) and only
// re-applies it when the hash changes; id/className mark Dirty::Style through the node setters.
enum class MetadataField { ElementId, ClassName, Role, Aria, Style };

struct ControlledInput;
struct DynamicPlan;

struct DynamicMetadata {
    unsigned ordinal = 0;
    MetadataField field = MetadataField::ElementId;
    std::string name; // the aria attribute name, for MetadataField::Aria
    const ir::Expr* value = nullptr;
};

// Emits the component header/source (emit_header.cpp / emit_component.cpp).
[[nodiscard]] bool emit_component_header(const ir::Component& component,
                                         std::string_view module_namespace,
                                         const std::vector<const ir::Binding*>& bindings,
                                         const std::vector<DynamicText>& dynamic_text,
                                         const std::vector<DynamicMetadata>& dynamic_metadata,
                                         const std::vector<ControlledInput>& controlled_inputs,
                                         const DynamicPlan& plan, GeneratedFile& file,
                                         std::string& error);

// Emits the effect deps-hash helpers and mount/update_effects/unmount (emit_lifecycle.cpp).
[[nodiscard]] bool emit_lifecycle(const ir::Component& component,
                                  const std::vector<const ir::Binding*>& bindings,
                                  CppWriter& writer, std::string& error);

// Appends `<Component>.hpp` and `<Component>.cpp` to `files`, or sets `error` and returns false.
[[nodiscard]] bool emit_component(const ir::Component& component, std::string_view module_namespace,
                                  const std::vector<ir::Binding>& bindings,
                                  std::vector<GeneratedFile>& files, std::string& error);

} // namespace tessera::compiler::codegen::internal
