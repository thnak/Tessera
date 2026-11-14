// SPDX-License-Identifier: MIT

// Helpers of the node emitter, split out of emit_node.cpp (docs/design/compiler.md section 11).

#include "emit_node_support.hpp"

#include <charconv>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace tessera::compiler::codegen::internal {

std::string node_comment(const ir::Node& node) {
    std::string out = "// " + node.id;
    if (!node.events.empty()) {
        out += " events={";
        bool first = true;
        for (const auto& [name, target] : node.events) {
            out += (first ? "" : ",") + name + ":" + target;
            first = false;
        }
        out += "}";
    }
    return out;
}

std::optional<std::string_view> literal_string(const ir::Expr* expr) {
    if (expr == nullptr || expr->kind != ir::ExprKind::Literal) {
        return std::nullopt;
    }
    if (const auto* text = std::get_if<std::string>(&expr->literal)) {
        return std::string_view(*text);
    }
    return std::nullopt;
}

std::optional<std::uint64_t> parse_automation_id(std::string_view text) {
    if (text.empty()) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

const ListTextSite* find_text_site(const std::vector<ListTextSite>* sites, unsigned ordinal) {
    if (sites == nullptr) {
        return nullptr;
    }
    for (const ListTextSite& site : *sites) {
        if (site.ordinal == ordinal) {
            return &site;
        }
    }
    return nullptr;
}

// `static_cast<tessera::ui::NodeId>(N)` for the static tree; inside a list-item builder the
// template's pre-order id is offset by the item index so every generated node instance has a unique
// NodeId (damage tracking keys previous bounds on it).
std::string node_id_text(const ir::Node& node, NodeEmitState& state, std::string& error) {
    std::string number;
    std::string suffix = "u";
    if (node.ui_id.empty()) {
        // Slice 10 computes uiId; until then the pre-order index is the deterministic placeholder.
        number = std::to_string(state.next_id++);
    } else {
        for (const char c : node.ui_id) {
            if (std::isdigit(static_cast<unsigned char>(c)) == 0) {
                error = "TESSERA_CODEGEN_UNSUPPORTED: node `" + node.id +
                        "` has a non-numeric uiId `" + node.ui_id + "`";
                return {};
            }
        }
        ++state.next_id;
        number = node.ui_id;
        suffix = "ull";
    }
    if (state.index_expr.empty()) {
        return "static_cast<tessera::ui::NodeId>(" + number + suffix + ")";
    }
    return "static_cast<tessera::ui::NodeId>(" + number + suffix + " + " + state.index_expr +
           " * kItemIdStride)";
}

} // namespace tessera::compiler::codegen::internal
