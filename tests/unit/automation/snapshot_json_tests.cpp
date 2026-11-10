// SPDX-License-Identifier: MIT

#include "tessera/ui/automation/snapshot_json.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>

namespace {

using tessera::ui::automation::build_snapshot;
using tessera::ui::automation::snapshot_to_json;
using tessera::ui::automation::SnapshotBounds;
using tessera::ui::automation::SnapshotOptions;
using tessera::ui::automation::SnapshotSourceNode;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "automation_snapshot_json: " << message << "\n";
    }
    return condition;
}

bool expect_contains(const std::string& text, std::string_view fragment, const char* message) {
    if (text.find(fragment) != std::string::npos) {
        return true;
    }
    std::cerr << "automation_snapshot_json: " << message << " (missing `" << fragment << "`)\n";
    return false;
}

SnapshotSourceNode make_tree() {
    SnapshotSourceNode root;
    root.ui_id = 100;
    root.component_id = 900;
    root.kind = "View";
    root.path = "root";
    root.name = "Counter";
    root.bounds = SnapshotBounds{1.0f, 2.0f, 30.0f, 40.0f};

    SnapshotSourceNode button;
    button.ui_id = std::numeric_limits<std::uint64_t>::max();
    button.component_id = 900;
    button.kind = "Button";
    button.path = "root/Button[0]";
    button.name = "Go";
    button.text = "Go";
    button.bounds = SnapshotBounds{0.0f, 0.0f, 10.0f, 10.0f};
    button.focused = true;

    root.children.push_back(button);
    return root;
}

} // namespace

int main() {
    const std::string json =
        snapshot_to_json(build_snapshot(make_tree(), SnapshotOptions{7, false}));

    // Canonical bytes: one line, fixed field order, uint64 ids as decimal strings, fixed-precision
    // bounds, trailing newline.
    const std::string expected =
        "{\"snapshotVersion\":1,\"sequence\":\"7\",\"nodes\":["
        "{\"uiId\":\"100\",\"componentId\":\"900\",\"parent\":null,\"depth\":0,\"itemKey\":\"\","
        "\"kind\":\"View\",\"path\":\"root\",\"name\":\"Counter\",\"text\":\"\","
        "\"bounds\":{\"x\":1.000,\"y\":2.000,\"width\":30.000,\"height\":40.000},"
        "\"visible\":true,\"enabled\":true,\"focused\":false,\"selected\":false},"
        "{\"uiId\":\"18446744073709551615\",\"componentId\":\"900\",\"parent\":0,\"depth\":1,"
        "\"itemKey\":\"\",\"kind\":\"Button\",\"path\":\"root/Button[0]\",\"name\":\"Go\","
        "\"text\":\"Go\",\"bounds\":{\"x\":0.000,\"y\":0.000,\"width\":10.000,\"height\":10.000},"
        "\"visible\":true,\"enabled\":true,\"focused\":true,\"selected\":false}"
        "]}\n";
    if (!expect(json == expected, "canonical snapshot json")) {
        std::cerr << "  got: " << json << "  want: " << expected;
        return 1;
    }

    // Determinism: the same tree serializes byte-for-byte identically.
    if (!expect(snapshot_to_json(build_snapshot(make_tree(), SnapshotOptions{7, false})) == json,
                "repeated serialization equal")) {
        return 2;
    }

    // Strings are escaped; non-ASCII UTF-8 passes through; no raw control bytes remain.
    SnapshotSourceNode escaped;
    escaped.ui_id = 1;
    escaped.name = "a\"b\\c\nd\te";
    escaped.name.push_back('\x01');
    escaped.name += "f";
    escaped.name += "\xC3\xA9";
    const std::string escaped_json = snapshot_to_json(build_snapshot(escaped));
    if (!expect_contains(escaped_json, "\"name\":\"a\\\"b\\\\c\\nd\\te\\u0001f\xC3\xA9\"",
                         "string escaping") ||
        !expect(escaped_json.find('\x01') == std::string::npos, "no raw control byte") ||
        !expect(escaped_json.find('\n') == escaped_json.size() - 1, "only the trailing newline")) {
        return 3;
    }

    // Bounds are fixed three-decimal values, rounded half away from zero and clamped.
    SnapshotSourceNode bounds_node;
    bounds_node.ui_id = 2;
    bounds_node.bounds = SnapshotBounds{-1.25f, 0.0004f, 1.0e-9f, 1234.5678f};
    const std::string bounds_json = snapshot_to_json(build_snapshot(bounds_node));
    if (!expect_contains(bounds_json,
                         "\"bounds\":{\"x\":-1.250,\"y\":0.000,\"width\":0.000,"
                         "\"height\":1234.568}",
                         "fixed bounds formatting")) {
        return 4;
    }

    // Hidden/offscreen nodes are absent unless included, and then carry visible=false.
    SnapshotSourceNode hidden_root = make_tree();
    hidden_root.children[0].visible = false;
    const std::string without_hidden = snapshot_to_json(build_snapshot(hidden_root));
    const std::string with_hidden =
        snapshot_to_json(build_snapshot(hidden_root, SnapshotOptions{0, true}));
    if (!expect(without_hidden.find("\"depth\":1") == std::string::npos, "hidden node omitted") ||
        !expect_contains(with_hidden, "\"visible\":false", "hidden node flagged when included")) {
        return 5;
    }

    // Item keys and state flags are emitted in the fixed order.
    SnapshotSourceNode list;
    list.ui_id = 3;
    list.item_key = "row-7";
    list.enabled = false;
    list.selected = true;
    const std::string list_json = snapshot_to_json(build_snapshot(list));
    if (!expect_contains(list_json, "\"itemKey\":\"row-7\"", "item key emitted") ||
        !expect_contains(list_json, "\"enabled\":false", "enabled flag") ||
        !expect_contains(list_json, "\"selected\":true", "selected flag")) {
        return 6;
    }

    // An empty snapshot is still valid canonical JSON.
    if (!expect(snapshot_to_json(build_snapshot(SnapshotSourceNode{})) ==
                    "{\"snapshotVersion\":1,\"sequence\":\"0\",\"nodes\":["
                    "{\"uiId\":\"0\",\"componentId\":\"0\",\"parent\":null,\"depth\":0,"
                    "\"itemKey\":\"\",\"kind\":\"\",\"path\":\"\",\"name\":\"\",\"text\":\"\","
                    "\"bounds\":{\"x\":0.000,\"y\":0.000,\"width\":0.000,\"height\":0.000},"
                    "\"visible\":true,\"enabled\":true,\"focused\":false,\"selected\":false}"
                    "]}\n",
                "default node json")) {
        return 7;
    }

    return 0;
}
