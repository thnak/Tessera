// SPDX-License-Identifier: MIT

#pragma once

#include "codegen_internal.hpp"
#include "emit_dynamic.hpp"
#include "emit_node.hpp"

#include <cstdint>
#include <optional>
#include <string_view>

// Small helpers of the node emitter (docs/design/compiler.md section 11): the deterministic node
// comment, literal-string and automation-id decoding, and the list-template text-site lookup. They
// live in their own translation unit so emit_node.cpp stays the node-walk emitter.
namespace tessera::compiler::codegen::internal {

// Metadata and event targets that still have no runtime API are recorded in a deterministic
// comment.
[[nodiscard]] std::string node_comment(const ir::Node& node);

// The string literal of `expr`, or nullopt when it is not a string literal.
[[nodiscard]] std::optional<std::string_view> literal_string(const ir::Expr* expr);

// The decimal `automation_id` of a node, or nullopt when it is empty or not a plain integer.
[[nodiscard]] std::optional<std::uint64_t> parse_automation_id(std::string_view text);

// The item-builder text site for `ordinal`, or nullptr when the node is not an interpolated
// template text node.
[[nodiscard]] const ListTextSite* find_text_site(const std::vector<ListTextSite>* sites,
                                                 unsigned ordinal);

} // namespace tessera::compiler::codegen::internal
