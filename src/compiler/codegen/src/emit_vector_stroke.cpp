// SPDX-License-Identifier: MIT

#include "emit_vector_stroke.hpp"

#include <initializer_list>
#include <optional>
#include <utility>
#include <variant>

namespace tessera::compiler::codegen::internal {
namespace {

bool fail(std::string& error, std::string message) {
    error = "TESSERA_CODEGEN_UNSUPPORTED: " + std::move(message);
    return false;
}

bool read_enum(const ir::Node& node, const char* prop,
               std::initializer_list<std::string_view> allowed, std::string_view& out,
               std::string& error) {
    const auto found = node.props.find(prop);
    if (found == node.props.end() || found->second == nullptr) {
        return true;
    }
    const ir::Expr& value = *found->second;
    const auto* text =
        value.kind == ir::ExprKind::Literal ? std::get_if<std::string>(&value.literal) : nullptr;
    if (text == nullptr) {
        return fail(error, "`Path` node `" + node.id + "` has a non-literal `" + prop +
                               "`; only a string literal is supported");
    }
    for (const std::string_view candidate : allowed) {
        if (*text == candidate) {
            out = candidate;
            return true;
        }
    }
    std::string list;
    for (const std::string_view candidate : allowed) {
        list += (list.empty() ? "\"" : ", \"") + std::string(candidate) + "\"";
    }
    return fail(error, "`Path` node `" + node.id + "` has `" + prop + "` `" + *text +
                           "`; expected one of " + list);
}

void append_attribute(std::string& svg, std::string_view name, std::string_view value) {
    if (value.empty()) {
        return;
    }
    svg += ' ';
    svg += name;
    svg += "=\"";
    svg += value;
    svg += '"';
}

} // namespace

bool read_path_extras(const ir::Node& node, PathExtras& extras, std::string& error) {
    return read_enum(node, "strokeLinecap", {"butt", "round", "square"}, extras.linecap, error) &&
           read_enum(node, "strokeLinejoin", {"miter", "round", "bevel"}, extras.linejoin, error) &&
           read_enum(node, "fillRule", {"nonzero", "evenodd"}, extras.fill_rule, error);
}

void append_path_extras(const PathExtras& extras, std::string& svg) {
    append_attribute(svg, "stroke-linecap", extras.linecap);
    append_attribute(svg, "stroke-linejoin", extras.linejoin);
    append_attribute(svg, "fill-rule", extras.fill_rule);
}

} // namespace tessera::compiler::codegen::internal
