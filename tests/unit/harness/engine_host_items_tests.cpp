// SPDX-License-Identifier: MIT
//
// (uiId, itemKey) list-item identity through the wired host: the snapshot exposes each realized
// row's key in document order, and click/focus/scroll resolve a row through (uiId, itemKey).
// Unknown, malformed, hidden and mismatched keys fail with documented codes.

#include "engine_host_support.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <cstddef>
#include <iostream>
#include <string>
#include <string_view>

namespace {

namespace harness = tessera::harness;
namespace automation = tessera::automation;
namespace testing = tessera::harness::testing;
namespace ui = tessera::ui;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "engine_host_items: " << message << "\n";
        ++failures;
    }
    return condition;
}

harness::ActionCommand action(harness::ActionKind kind, std::uint64_t ui_id, std::string_view key) {
    harness::ActionCommand command;
    command.kind = kind;
    command.ui_id = ui_id;
    command.item_key = std::string(key);
    return command;
}

ui::Node* row_node(ui::Node& root, std::uint64_t key) {
    ui::Node* list = testing::find_node(root, automation::kFixtureListId);
    if (list == nullptr) {
        return nullptr;
    }
    for (const auto& child : list->children()) {
        if (child->item_key() == key) {
            return child.get();
        }
    }
    return nullptr;
}

void snapshot_exposes_keys() {
    testing::HostFixture scene;
    const auto snapshot = scene.automation.canonical_snapshot(false);
    if (!expect(snapshot.has_value(), "snapshot succeeds")) {
        return;
    }
    const auto* nodes = snapshot->find("nodes");
    const auto* values = nodes == nullptr ? nullptr : nodes->as_array();
    if (!expect(values != nullptr, "nodes array present")) {
        return;
    }
    std::string order;
    for (const harness::JsonValue& node : *values) {
        const auto* id = node.find("uiId");
        const auto* key = node.find("itemKey");
        if (id != nullptr && id->as_string() == "9010" && key != nullptr) {
            order += std::string(*key->as_string());
            order += ',';
        }
    }
    expect(order == "101,102,103,", "rows expose their keys in document order");
    const auto* plain = testing::snapshot_node(*snapshot, "9003");
    const auto* plain_key = plain == nullptr ? nullptr : plain->find("itemKey");
    expect(plain_key != nullptr && plain_key->as_string() == std::string_view(),
           "a non-list node has an empty itemKey");
}

void actions_resolve_items() {
    testing::HostFixture scene;
    auto focus = scene.automation.dispatch_action(
        action(harness::ActionKind::Focus, automation::kFixtureRowId, "102"));
    expect(focus.has_value(), "focus by (uiId, itemKey) succeeds");
    expect(scene.automation.input().router().focus_manager().focused() == row_node(scene.root, 102),
           "focus lands on the keyed instance, not the first row");

    auto click = scene.automation.dispatch_action(
        action(harness::ActionKind::Click, automation::kFixtureRowId, "103"));
    expect(click.has_value(), "click by (uiId, itemKey) succeeds");
    expect(scene.automation.input().router().focus_manager().focused() == row_node(scene.root, 103),
           "click focuses the keyed instance");

    auto scroll = scene.automation.dispatch_action(
        action(harness::ActionKind::Scroll, automation::kFixtureRowId, "101"));
    expect(scroll.has_value(), "scroll by (uiId, itemKey) succeeds");
}

void failures_are_clear() {
    testing::HostFixture scene;
    auto missing = scene.automation.dispatch_action(
        action(harness::ActionKind::Click, automation::kFixtureRowId, "999"));
    expect(!missing.has_value() && missing.error().code == harness::ProtocolErrorCode::UiIdNotFound,
           "an unknown key is uiIdNotFound");
    expect(!missing.has_value() && missing.error().detail == "9010/999",
           "the detail names (uiId, itemKey)");

    auto wrong_id = scene.automation.dispatch_action(
        action(harness::ActionKind::Click, automation::kFixtureButtonId, "101"));
    expect(!wrong_id.has_value() &&
               wrong_id.error().code == harness::ProtocolErrorCode::UiIdNotFound,
           "a key on the wrong uiId does not match");

    for (const char* bad : {"row-1", "", "+101", "0101", "101 ", "18446744073709551616"}) {
        auto result = scene.automation.dispatch_action(
            action(harness::ActionKind::Click, automation::kFixtureRowId, bad));
        expect(!result.has_value() &&
                   result.error().code == harness::ProtocolErrorCode::InvalidArgument,
               "a non-canonical key is invalidArgument");
    }

    ui::Node* row = row_node(scene.root, 102);
    if (expect(row != nullptr, "row 102 exists")) {
        row->set_visible(false);
        auto hidden = scene.automation.dispatch_action(
            action(harness::ActionKind::Focus, automation::kFixtureRowId, "102"));
        expect(!hidden.has_value() &&
                   hidden.error().code == harness::ProtocolErrorCode::UiIdNotFound,
               "a hidden item is not targetable");
    }
}

void scroll_into_view_reveals_item() {
    testing::HostFixture scene;
    ui::Node* list = testing::find_node(scene.root, automation::kFixtureListId);
    ui::Node* row = row_node(scene.root, 101);
    if (!expect(list != nullptr && row != nullptr, "list and row 101 exist")) {
        return;
    }
    // Push the list to its end so the first row sits above the scrollport.
    (void)ui::scroll_to(*list, 0.0f, ui::max_scroll_y(*list));
    const float scrolled_away = list->scroll_y();
    if (!expect(scrolled_away > 0.0f, "the fixture list can scroll")) {
        return;
    }
    const float row_top_before = row->world_bounds().y;

    auto reveal = scene.automation.dispatch_action(
        action(harness::ActionKind::ScrollIntoView, automation::kFixtureRowId, "101"));
    expect(reveal.has_value(), "scrollIntoView by (uiId, itemKey) succeeds");
    expect(list->scroll_y() < scrolled_away, "the list scrolled back toward the item");
    expect(row->world_bounds().y > row_top_before, "the item moved into the viewport");
    expect(row->world_bounds().y >= list->world_bounds().y, "the item top is inside the list");

    // Already visible: idempotent success with no offset change.
    const float settled = list->scroll_y();
    auto again = scene.automation.dispatch_action(
        action(harness::ActionKind::ScrollIntoView, automation::kFixtureRowId, "101"));
    expect(again.has_value() && list->scroll_y() == settled, "scrollIntoView is idempotent");

    auto missing = scene.automation.dispatch_action(
        action(harness::ActionKind::ScrollIntoView, automation::kFixtureRowId, "999"));
    expect(!missing.has_value() && missing.error().code == harness::ProtocolErrorCode::UiIdNotFound,
           "an unknown item is uiIdNotFound");
}

} // namespace

int main() {
    snapshot_exposes_keys();
    actions_resolve_items();
    failures_are_clear();
    scroll_into_view_reveals_item();
    return failures == 0 ? 0 : 1;
}
