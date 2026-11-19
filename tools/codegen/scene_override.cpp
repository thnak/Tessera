// SPDX-License-Identifier: MIT

#include "scene_override.hpp"
#include "scene_stylesheet.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/cascade.hpp"
#include "tessera/ui/style/prepared_stylesheet.hpp"

#include <algorithm>
#include <utility>

namespace tessera::compiler::codegen::cli {
namespace {

namespace style = tessera::ui::style;

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n\f");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n\f");
    return text.substr(first, last - first + 1);
}

OverrideDiagnostic parse_error(std::string message) {
    return OverrideDiagnostic{"TESSERA_CSS_PARSE_ERROR", std::move(message)};
}

} // namespace

std::optional<OverrideDiagnostic> validate_override(std::string_view property,
                                                    std::string_view value) {
    property = trim(property);
    value = trim(value);
    if (property.empty()) {
        return parse_error("an override needs a property name");
    }
    if (value.empty()) {
        return parse_error("an override of `" + std::string(property) + "` needs a value");
    }
    // The scene keeps an override as an inline declaration with a bounded name and value; a longer
    // one would be truncated silently, which would paint a value the developer did not type.
    if (property.size() > tessera::ui::Node::kMaxInlineStyleNameBytes ||
        value.size() > tessera::ui::Node::kMaxInlineStyleValueBytes) {
        return OverrideDiagnostic{"TESSERA_DESIGNER_LIMIT",
                                  "the declaration exceeds the override size limit"};
    }
    // Reject anything that could end the declaration or the rule before the parser sees it: the
    // one-rule sheet below is built by concatenation.
    const auto structural = [](char c) { return c == ';' || c == '{' || c == '}' || c == '\n'; };
    if (std::any_of(property.begin(), property.end(), structural) ||
        std::any_of(value.begin(), value.end(), structural) ||
        property.find(':') != std::string_view::npos) {
        return parse_error("`" + std::string(property) + ": " + std::string(value) +
                           "` is not one declaration");
    }

    std::string sheet = "x{";
    sheet += property;
    sheet += ':';
    sheet += value;
    sheet += '}';
    const style::PreparedStyleSheet prepared = style::prepare_stylesheet(sheet);
    if (!prepared.diagnostics.empty()) {
        const style::StyleDiagnostic& first = prepared.diagnostics.front();
        return OverrideDiagnostic{css_diagnostic_code(first.kind), first.message};
    }
    // A declaration the parser dropped without a diagnostic (a malformed one) leaves the rule with
    // no declarations, which is not a value the cascade would ever apply.
    bool has_declaration = false;
    for (const style::PreparedItem& item : prepared.items) {
        has_declaration = has_declaration || !item.declarations.empty();
    }
    if (!has_declaration) {
        return parse_error("`" + std::string(property) + ": " + std::string(value) +
                           "` is not a declaration");
    }
    return std::nullopt;
}

void OverrideLayer::push_history() {
    undo_.push_back(entries_);
    if (undo_.size() > kMaxHistory) {
        undo_.erase(undo_.begin());
    }
    redo_.clear();
}

bool OverrideLayer::set(std::uint64_t automation_id, std::string_view property,
                        std::string_view value) {
    const std::string name = style::normalize_property(trim(property));
    const std::string text(trim(value));
    for (const StyleOverride& entry : entries_) {
        if (entry.automation_id == automation_id && entry.property == name && entry.value == text) {
            return false;
        }
    }
    push_history();
    for (StyleOverride& entry : entries_) {
        if (entry.automation_id == automation_id && entry.property == name) {
            entry.value = text;
            return true;
        }
    }
    entries_.push_back(StyleOverride{automation_id, name, text});
    return true;
}

bool OverrideLayer::clear(std::uint64_t automation_id, std::string_view property) {
    const std::string name =
        property.empty() ? std::string() : style::normalize_property(trim(property));
    const auto matches = [&](const StyleOverride& entry) {
        return entry.automation_id == automation_id && (name.empty() || entry.property == name);
    };
    if (std::none_of(entries_.begin(), entries_.end(), matches)) {
        return false;
    }
    push_history();
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(), matches), entries_.end());
    return true;
}

bool OverrideLayer::revert() {
    if (entries_.empty()) {
        return false;
    }
    push_history();
    entries_.clear();
    return true;
}

bool OverrideLayer::undo() {
    if (undo_.empty()) {
        return false;
    }
    redo_.push_back(std::move(entries_));
    entries_ = std::move(undo_.back());
    undo_.pop_back();
    return true;
}

bool OverrideLayer::redo() {
    if (redo_.empty()) {
        return false;
    }
    undo_.push_back(std::move(entries_));
    entries_ = std::move(redo_.back());
    redo_.pop_back();
    return true;
}

void OverrideLayer::consume(std::uint64_t automation_id) {
    const auto of_node = [&](const StyleOverride& entry) {
        return entry.automation_id == automation_id;
    };
    const auto strip = [&](Snapshot& snapshot) {
        snapshot.erase(std::remove_if(snapshot.begin(), snapshot.end(), of_node), snapshot.end());
    };
    // Every snapshot of the history, then the live entries, lose this node. Walking the undo stack
    // oldest to newest and dropping a snapshot equal to the one after it leaves exactly the
    // operations that still change something: a `set` that only touched the committed node is now a
    // step from a layer to the same layer, and an undo that restored nothing would be a dead click.
    strip(entries_);
    for (Snapshot& snapshot : undo_) {
        strip(snapshot);
    }
    for (Snapshot& snapshot : redo_) {
        strip(snapshot);
    }
    const auto collapse = [](std::vector<Snapshot>& stack, const Snapshot& live) {
        // `stack` runs oldest to newest and `live` follows its last element.
        std::vector<Snapshot> kept;
        for (std::size_t index = 0; index < stack.size(); ++index) {
            const Snapshot& next = index + 1 < stack.size() ? stack[index + 1] : live;
            if (stack[index] != next) {
                kept.push_back(std::move(stack[index]));
            }
        }
        stack = std::move(kept);
    };
    collapse(undo_, entries_);
    // The redo stack runs newest to oldest after `live`, so it is collapsed in the other direction.
    std::reverse(redo_.begin(), redo_.end());
    collapse(redo_, entries_);
    std::reverse(redo_.begin(), redo_.end());
}

std::vector<StyleOverride> OverrideLayer::of(std::uint64_t automation_id) const {
    std::vector<StyleOverride> result;
    for (const StyleOverride& entry : entries_) {
        if (entry.automation_id == automation_id) {
            result.push_back(entry);
        }
    }
    return result;
}

} // namespace tessera::compiler::codegen::cli
