// SPDX-License-Identifier: MIT
//
// Shared scene for the overlay tests (issue #302): a real AppHost over a 400x800 window with one
// base content area, so layout, style and hit testing come from the shipped pipeline.

#pragma once

#include "a11y_test_helpers.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/overlay/overlay.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <string>

namespace overlay_test {

using tessera::ui::FocusManager;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::OverlayHandle;
using tessera::ui::OverlayHost;
using tessera::ui::OverlayInfo;
using tessera::ui::OverlayLayer;
using tessera::ui::OverlaySpec;
namespace key = tessera::ui::key;

inline int failures = 0;
inline const char* suite = "overlay";

inline bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << suite << ": " << message << "\n";
        ++failures;
    }
    return condition;
}

constexpr double kFrame = 1.0 / 60.0;
constexpr NodeId kBase = 2;
constexpr NodeId kBaseButton = 3;

// A 100x100 box at the top-left of whichever overlay content root it is mounted in.
inline void box(Node& content, NodeId id, bool focusable = false) {
    Node& node = content.add_child(std::make_unique<Node>(id));
    node.set_element_kind("View");
    node.set_class_name("box");
    node.set_focusable(focusable);
    node.set_tab_index(0);
}

inline OverlaySpec spec(const char* id, OverlayLayer layer, NodeId box_id, int priority = 0,
                        bool focusable = false) {
    OverlaySpec value;
    value.id = id;
    value.layer = layer;
    value.priority = priority;
    value.mount = [box_id, focusable](Node& content) { box(content, box_id, focusable); };
    return value;
}

// root -> base (fills the window, holds one focusable button).
struct Fixture {
    Fixture()
        : root(1),
          loop(clock) {
        Node& base = root.add_child(std::make_unique<Node>(kBase));
        base.set_element_kind("View");
        base.set_class_name("fill");
        Node& button = base.add_child(std::make_unique<Node>(kBaseButton));
        button.set_element_kind("View");
        button.set_class_name("fill");
        button.set_focusable(true);
        button.set_tab_index(0);
        tessera::app::HostConfig config;
        config.width = 400.0f;
        config.height = 800.0f;
        host = std::make_unique<tessera::app::AppHost>(
            root,
            tessera::ui::style::compile_stylesheet(
                ".fill { flex-grow: 1; display: flex; flex-direction: column; }"
                ".box { width: 100px; height: 100px; }"),
            loop, config);
    }

    tessera::app::HostFrame frame() {
        const auto result = host->begin_frame(kFrame);
        host->end_frame();
        return result;
    }

    // The deepest node under the point, as the pointer router would pick it.
    Node* hit(float x, float y) {
        return root.hit_test_deep(x, y);
    }

    tessera::runtime::ManualClock clock;
    Node root;
    tessera::runtime::RuntimeLoop loop;
    std::unique_ptr<tessera::app::AppHost> host;
};

inline bool has_id(const tessera::ui::a11y::A11ySnapshot& snapshot, NodeId id) {
    return std::ranges::any_of(snapshot.nodes, [id](const auto& node) { return node.id == id; });
}

} // namespace overlay_test
