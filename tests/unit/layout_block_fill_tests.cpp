// SPDX-License-Identifier: MIT

// A block child with `width: auto` and `margin: 0 auto` fills its container up to `max-width` and
// is centred, as in a browser (issue #611). Expected values are Chromium's for the same markup.

#include "tessera/ui/layout/layout_engine.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <tuple>
#include <utility>

namespace {

using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::ComputedStyle;

using Declarations = std::initializer_list<std::pair<const char*, const char*>>;

LayoutInput node(std::uint64_t id, Declarations values) {
    LayoutInput result;
    result.id = id;
    for (const auto& [property, value] : values) {
        result.style.set(property, value);
    }
    return result;
}

const LayoutBox* find(const tessera::ui::layout::LayoutResult& result, std::uint64_t id) {
    for (const LayoutBox& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

bool near(float actual, float expected) {
    return std::fabs(actual - expected) < 0.01f;
}

bool check(float viewport_width, Declarations app, float x, float width, const char* name) {
    LayoutInput root = node(1, app);
    root.children.push_back(node(2, {{"height", "10px"}}));
    LayoutViewport viewport;
    viewport.width = viewport_width;
    viewport.height = 600.0f;
    const LayoutEngine engine;
    const auto result = engine.layout(root, viewport);
    const LayoutBox* box = find(result, 1);
    const LayoutBox* child = find(result, 2);
    if (box == nullptr || child == nullptr || !near(box->bounds.x, x) ||
        !near(box->bounds.width, width)) {
        std::cerr << "layout_block_fill: " << name << ": got x=" << (box ? box->bounds.x : -1.0f)
                  << " width=" << (box ? box->bounds.width : -1) << ", expected x=" << x
                  << " width=" << width << "\n";
        return false;
    }
    return true;
}

} // namespace

int main() {
    bool ok = true;
    // `.app { max-width: 880px; margin: 0 auto; padding: 32px 24px }` in a block parent.
    const Declarations app{{"display", "block"},     {"max-width", "880px"},
                           {"margin-left", "auto"},  {"margin-right", "auto"},
                           {"padding-left", "24px"}, {"padding-right", "24px"}};
    // Content-box: 880px of content plus 48px of padding caps the border box at 928px, so at 900
    // it fills (Chromium: x=0, width 900); at 1200 it is capped and centred.
    ok &= check(900.0f, app, 0.0f, 900.0f, "900 fills");
    ok &= check(1200.0f, app, 136.0f, 928.0f, "1200 centred");
    ok &= check(400.0f, app, 0.0f, 400.0f, "400 fills, not wider than the viewport");

    // No auto margin: untouched behaviour (a stretched block).
    ok &= check(900.0f, {{"display", "block"}, {"max-width", "500px"}}, 0.0f, 500.0f,
                "max-width without auto margin");
    // An explicit width keeps the author's size and still centres.
    ok &= check(900.0f,
                {{"display", "block"},
                 {"width", "300px"},
                 {"margin-left", "auto"},
                 {"margin-right", "auto"}},
                300.0f, 300.0f, "explicit width centres");

    // Nested in a block parent: capped and centred inside it, and filling a narrow one.
    for (const auto& [width, x, expected] :
         {std::tuple{900.0f, 200.0f, 500.0f}, std::tuple{300.0f, 0.0f, 300.0f}}) {
        LayoutInput parent = node(1, {{"display", "block"}});
        parent.children.push_back(node(2, {{"display", "block"},
                                           {"max-width", "500px"},
                                           {"margin-left", "auto"},
                                           {"margin-right", "auto"},
                                           {"height", "10px"}}));
        LayoutViewport viewport;
        viewport.width = width;
        const auto result = LayoutEngine().layout(parent, viewport);
        const LayoutBox* box = find(result, 2);
        if (box == nullptr || !near(box->bounds.x, x) || !near(box->bounds.width, expected)) {
            std::cerr << "layout_block_fill: nested at " << width
                      << ": got x=" << (box ? box->bounds.x : -1.0f)
                      << " width=" << (box ? box->bounds.width : -1.0f) << '\n';
            ok = false;
        }
    }

    // The root is at least the viewport tall and grows with taller content (issue #611).
    for (const auto& [content, expected] :
         {std::pair{1000.0f, 1000.0f}, std::pair{100.0f, 600.0f}}) {
        LayoutInput page = node(1, {{"display", "block"}, {"padding-top", "10px"}});
        page.children.push_back(node(2, {{"height", content == 1000.0f ? "990px" : "100px"}}));
        LayoutViewport viewport;
        viewport.width = 400.0f;
        viewport.height = 600.0f;
        const auto result = LayoutEngine().layout(page, viewport);
        const LayoutBox* box = find(result, 1);
        if (box == nullptr || !near(box->bounds.height, expected)) {
            std::cerr << "layout_block_fill: root height for content " << content << ": got "
                      << (box ? box->bounds.height : -1.0f) << ", expected " << expected << '\n';
            ok = false;
        }
    }
    return ok ? 0 : 1;
}
