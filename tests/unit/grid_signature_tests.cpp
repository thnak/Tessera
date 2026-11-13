// SPDX-License-Identifier: MIT
//
// Layout-signature and dirty coverage for the CSS Grid longhands (issue #249): every grid property
// that can move a box must change the signature, and a paint-only change must not. The scheduler
// then reuses boxes for a paint-only change and reflows for a grid change, which is the dirty
// contract of docs/design/dirty-rendering.md applied to the grid domain.

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_scheduler.hpp"
#include "tessera/ui/layout/layout_signature.hpp"

#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

namespace {

using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutScheduler;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "grid_signature: " << message << "\n";
    }
    return condition;
}

ComputedStyle style(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle result;
    for (const auto& [property, value] : values) {
        result.set(property, value);
    }
    return result;
}

LayoutInput node(std::uint64_t id,
                 std::initializer_list<std::pair<const char*, const char*>> values) {
    LayoutInput result;
    result.id = id;
    result.style = style(values);
    return result;
}

LayoutInput grid_scene(std::initializer_list<std::pair<const char*, const char*>> container,
                       std::initializer_list<std::pair<const char*, const char*>> item) {
    LayoutInput root = node(1, container);
    root.children.push_back(node(2, item));
    root.children.push_back(node(3, {}));
    return root;
}

const LayoutViewport kViewport{400.0f, 200.0f};

std::uint64_t signature_of(const LayoutInput& root) {
    return tessera::ui::layout::layout_signature(root, kViewport);
}

bool same_boxes(const LayoutResult& lhs, const LayoutResult& rhs) {
    if (lhs.boxes.size() != rhs.boxes.size()) {
        return false;
    }
    for (std::size_t i = 0; i < lhs.boxes.size(); ++i) {
        const auto& a = lhs.boxes[i];
        const auto& b = rhs.boxes[i];
        if (a.id != b.id || a.bounds.x != b.bounds.x || a.bounds.y != b.bounds.y ||
            a.bounds.width != b.bounds.width || a.bounds.height != b.bounds.height) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    const LayoutInput base = grid_scene({{"display", "grid"},
                                         {"width", "400px"},
                                         {"height", "200px"},
                                         {"grid-template-columns", "100px 100px"},
                                         {"grid-template-rows", "50px 50px"}},
                                        {{"grid-column", "1"}, {"grid-row", "1"}});
    const std::uint64_t base_signature = signature_of(base);

    // Every grid longhand that can move a box is part of the signature.
    struct Probe {
        const char* property;
        const char* value;
        bool on_container;
    };
    const Probe probes[] = {
        {"grid-template-columns", "1fr 1fr", true},
        {"grid-template-rows", "100px", true},
        {"grid-auto-flow", "column", true},
        {"justify-items", "center", true},
        {"align-items", "center", true},
        {"justify-content", "center", true},
        {"align-content", "center", true},
        {"column-gap", "10px", true},
        {"row-gap", "10px", true},
        {"direction", "rtl", true},
        {"grid-column", "2", false},
        {"grid-row", "2", false},
        {"justify-self", "center", false},
        {"align-self", "center", false},
    };
    for (const Probe& probe : probes) {
        LayoutInput probe_scene = base;
        ComputedStyle& target =
            probe.on_container ? probe_scene.style : probe_scene.children.front().style;
        target.set(probe.property, probe.value);
        if (!expect(signature_of(probe_scene) != base_signature,
                    "grid property changes the signature")) {
            std::cerr << "  property: " << probe.property << "\n";
            return 1;
        }
    }

    // A paint-only change must not invalidate layout.
    LayoutInput paint_only = base;
    paint_only.children.front().style.set("background-color", "#123456");
    paint_only.children.front().style.set("opacity", "0.5");
    if (!expect(signature_of(paint_only) == base_signature,
                "paint-only change keeps the signature")) {
        return 2;
    }

    // The scheduler reuses boxes for a paint-only change and reflows for a grid change.
    LayoutEngine engine;
    LayoutScheduler scheduler(engine);
    const LayoutResult& first = scheduler.update(base, kViewport, true);
    const LayoutResult first_copy = first;
    if (!expect(scheduler.layout_runs() == 1, "first update reflows")) {
        return 3;
    }
    const LayoutResult& reused = scheduler.update(paint_only, kViewport, true);
    if (!expect(scheduler.layout_runs() == 1 && scheduler.cache_hits() == 1,
                "paint-only update is a cache hit") ||
        !expect(same_boxes(reused, first_copy), "cache hit returns the same boxes")) {
        return 4;
    }

    LayoutInput moved = base;
    moved.children.front().style.set("grid-column", "2");
    const LayoutResult& reflowed = scheduler.update(moved, kViewport, true);
    if (!expect(scheduler.layout_runs() == 2, "grid change reflows")) {
        return 5;
    }
    const auto* moved_box = [&reflowed]() -> const tessera::ui::layout::LayoutBox* {
        for (const auto& box : reflowed.boxes) {
            if (box.id == 2) {
                return &box;
            }
        }
        return nullptr;
    }();
    if (!expect(moved_box != nullptr && moved_box->bounds.x == 100.0f,
                "grid-column: 2 moves the item")) {
        return 6;
    }

    // A grid item's text is a layout input: changing it reflows.
    LayoutInput text_scene =
        node(10, {{"display", "grid"}, {"width", "200px"}, {"grid-template-columns", "100px"}});
    LayoutInput text_item = node(11, {{"justify-self", "start"}});
    text_item.text = "hello";
    text_scene.children.push_back(text_item);
    const std::uint64_t text_signature = signature_of(text_scene);
    text_scene.children.front().text = "hello world";
    if (!expect(signature_of(text_scene) != text_signature, "grid item text is a layout input")) {
        return 7;
    }

    std::cout << "grid_signature: ok\n";
    return 0;
}
