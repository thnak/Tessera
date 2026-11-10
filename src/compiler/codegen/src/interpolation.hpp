// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "emit_controlled.hpp"
#include "emit_dynamic.hpp"
#include "value_ops.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Text interpolation and the generated update path (docs/design/compiler.md section 11). The value
// typing/hash helpers live in value_ops.hpp so the keyed-list item content pass shares them.
namespace tessera::compiler::codegen::internal {

// Walks the node tree in the same pre-order as emit_node and collects the nodes whose text is not a
// static literal (a string literal or an all-text template). A list template subtree is skipped:
// its text is per item and its interpolation goes through the generated per-item content pass. Sets
// `error` for a text expression with no printable C++ type.
[[nodiscard]] bool collect_dynamic_text(const ir::Node& node, const ir::Component& component,
                                        unsigned& ordinal, std::vector<DynamicText>& entries,
                                        std::string& error);

// A static text payload: a string literal or an all-text template folded by codegen.
[[nodiscard]] std::optional<std::string> static_text_value(const ir::Expr* expr);

// Emits the per-file `format_value` helpers into the generated .cpp (anonymous namespace).
void emit_format_helpers(CppWriter& writer);

// Walks the node tree in pre-order and collects interpolated element metadata values. List template
// subtrees are skipped (dynamic metadata inside a template is rejected loudly by the plan).
[[nodiscard]] bool collect_dynamic_metadata(const ir::Node& node, const ir::Component& component,
                                            unsigned& ordinal,
                                            std::vector<DynamicMetadata>& entries,
                                            std::string& error);

// The generated member base for one dynamic metadata field (`meta_3_class`).
[[nodiscard]] std::string metadata_member(const DynamicMetadata& entry);

// Emits `<Name>::apply_state()`: the instance props first, then the dynamic child-set groups, then
// interpolated text and metadata, then the controlled-editor sync. `has_props` emits the
// `apply_props()` call that refreshes the declared props of the owned component instances.
[[nodiscard]] bool emit_apply_state(CppWriter& writer, const ir::Component& component,
                                    const std::vector<DynamicText>& text_entries,
                                    const std::vector<DynamicMetadata>& metadata_entries,
                                    const std::vector<ControlledInput>& controlled_inputs,
                                    const DynamicPlan& plan, bool has_props, std::string& error);

} // namespace tessera::compiler::codegen::internal
