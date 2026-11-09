// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Text interpolation and the generated update path (docs/design/compiler.md section 11).
namespace tessera::compiler::codegen::internal {

// The C++ type family a text interpolation expression produces.
enum class TextValueType { Bool, Int, Float, String };

// Walks the node tree in the same pre-order as emit_node and collects the nodes whose text is not a
// static literal (a string literal or an all-text template). Sets `error` for a text expression
// with no printable C++ type.
[[nodiscard]] bool collect_dynamic_text(const ir::Node& node, const ir::Component& component,
                                        unsigned& ordinal, std::vector<DynamicText>& entries,
                                        std::string& error);

// A static text payload: a string literal or an all-text template folded by codegen.
[[nodiscard]] std::optional<std::string> static_text_value(const ir::Expr* expr);

// Emits the per-file `format_value` helpers into the generated .cpp (anonymous namespace).
void emit_format_helpers(CppWriter& writer);

// Walks the node tree in pre-order and collects interpolated element metadata values.
[[nodiscard]] bool collect_dynamic_metadata(const ir::Node& node, const ir::Component& component,
                                            unsigned& ordinal,
                                            std::vector<DynamicMetadata>& entries,
                                            std::string& error);

// The generated member base for one dynamic metadata field (`meta_3_class`).
[[nodiscard]] std::string metadata_member(const DynamicMetadata& entry);

// Emits `<Name>::apply_state()` for the collected dynamic text and metadata entries.
[[nodiscard]] bool emit_apply_state(CppWriter& writer, const ir::Component& component,
                                    const std::vector<DynamicText>& text_entries,
                                    const std::vector<DynamicMetadata>& metadata_entries,
                                    std::string& error);

} // namespace tessera::compiler::codegen::internal
