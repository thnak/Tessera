// SPDX-License-Identifier: MIT
//
// List-item targeting for EngineAutomationHost (docs/specs/automation-ids-v1.md section 1,
// headless-automation-v1.md section 6.8). A realized virtual item keeps its template UiId and
// carries its data-lifetime key on the scene node, so `(uiId, itemKey)` resolves to one instance.
// Unrealized items have no scene node and therefore cannot be targeted.

#include "tessera/automation/engine_host.hpp"

#include <charconv>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace tessera::automation {
namespace {

namespace harness = tessera::harness;
namespace ui = tessera::ui;

harness::ProtocolError error(harness::ProtocolErrorCode code, std::string message,
                             std::string detail) {
    return harness::ProtocolError{code, std::move(message), std::move(detail), std::nullopt};
}

// Canonical unsigned decimal only (no sign, no leading zeros, no whitespace): the same text the
// snapshot emits, so a key copied from a snapshot always resolves and a lookalike never does.
std::optional<std::uint64_t> parse_item_key(std::string_view text) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

ui::Node* find_visible_item(ui::Node& node, harness::UiId ui_id, std::uint64_t key) {
    if (!node.visible()) {
        return nullptr;
    }
    if (node.automation_id() == ui_id && node.item_key() == key) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (ui::Node* found = find_visible_item(*child, ui_id, key); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

std::expected<ui::Node*, harness::ProtocolError>
EngineAutomationHost::resolve_target(harness::UiId ui_id,
                                     const std::optional<std::string>& item_key) const {
    if (!item_key.has_value()) {
        ui::Node* target = find_target(ui_id);
        if (target == nullptr) {
            return std::unexpected(error(harness::ProtocolErrorCode::UiIdNotFound,
                                         "no visible node matches the automation id",
                                         std::to_string(ui_id)));
        }
        return target;
    }
    const auto key = parse_item_key(*item_key);
    if (!key.has_value()) {
        return std::unexpected(error(harness::ProtocolErrorCode::InvalidArgument,
                                     "itemKey must be canonical decimal text", *item_key));
    }
    ui::Node* target = ui_id == 0 ? nullptr : find_visible_item(root_, ui_id, *key);
    if (target == nullptr) {
        return std::unexpected(error(harness::ProtocolErrorCode::UiIdNotFound,
                                     "no visible realized list item matches (uiId, itemKey)",
                                     std::to_string(ui_id) + "/" + *item_key));
    }
    return target;
}

} // namespace tessera::automation
