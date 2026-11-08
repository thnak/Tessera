// SPDX-License-Identifier: MIT

#include "tessera/ui/layout/layout_scheduler.hpp"

#include <initializer_list>
#include <iostream>
#include <utility>

namespace {

using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutScheduler;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "layout_scheduler: " << message << "\n";
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

const tessera::ui::layout::LayoutBox* find_box(const LayoutResult& result, std::uint64_t id) {
    for (const auto& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

} // namespace

int main() {
    tessera::ui::layout::LayoutEngine engine;
    LayoutScheduler scheduler(engine);

    LayoutInput root;
    root.id = 1;
    root.style = style({{"width", "200px"}, {"height", "100px"}});
    LayoutInput child;
    child.id = 2;
    child.style = style(
        {{"width", "50px"}, {"height", "50px"}, {"margin-left", "30px"}, {"margin-top", "10px"}});
    root.children.push_back(child);

    LayoutViewport viewport;
    viewport.width = 320.0f;
    viewport.height = 200.0f;

    const LayoutResult result = scheduler.update(root, viewport, true);
    if (!expect(scheduler.valid(), "first layout is valid") ||
        !expect(scheduler.layout_runs() == 1, "first update runs layout") ||
        !expect(result.boxes.size() == 2, "root and child boxes")) {
        return 1;
    }

    const LayoutResult cached = scheduler.update(root, viewport, false);
    if (!expect(scheduler.layout_runs() == 1, "clean paint frame skips layout") ||
        !expect(scheduler.cache_hits() == 1, "cache hit counted") ||
        !expect(cached.boxes.size() == 2, "cached boxes returned")) {
        return 2;
    }

    // A size change in an independent subtree patches only its boxes and keeps its parent offset.
    root.children[0].style = style(
        {{"width", "70px"}, {"height", "60px"}, {"margin-left", "30px"}, {"margin-top", "10px"}});
    if (!expect(scheduler.relayout_subtree(root, 2, viewport), "subtree relayout applied") ||
        !expect(scheduler.partial_runs() == 1, "partial run counted") ||
        !expect(scheduler.layout_runs() == 1, "partial run does not relayout the root")) {
        return 3;
    }
    const auto* patched = find_box(scheduler.current(), 2);
    const auto* still_root = find_box(scheduler.current(), 1);
    if (!expect(patched != nullptr && patched->bounds.width == 70.0f &&
                    patched->bounds.height == 60.0f,
                "patched subtree box size") ||
        !expect(patched->bounds.x == 30.0f && patched->bounds.y == 10.0f,
                "patched subtree keeps parent offset") ||
        !expect(still_root != nullptr && still_root->bounds.width == 200.0f,
                "root box untouched")) {
        return 4;
    }
    if (!expect(!scheduler.relayout_subtree(root, 99, viewport), "unknown subtree id rejected")) {
        return 5;
    }

    (void)scheduler.update(root, viewport, true);
    if (!expect(scheduler.layout_runs() == 2, "layout dirt recomputes")) {
        return 6;
    }

    LayoutViewport resized = viewport;
    resized.width = 480.0f;
    (void)scheduler.update(root, resized, false);
    if (!expect(scheduler.layout_runs() == 3, "viewport change recomputes without dirt flag")) {
        return 7;
    }

    scheduler.invalidate();
    if (!expect(!scheduler.valid(), "invalidate drops the cache")) {
        return 8;
    }
    (void)scheduler.update(root, resized, false);
    if (!expect(scheduler.layout_runs() == 4, "invalidated cache recomputes")) {
        return 9;
    }

    std::cout << "layout_scheduler: ok\n";
    return 0;
}
