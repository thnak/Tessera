// SPDX-License-Identifier: MIT
//
// The direction contract end to end (stylesheet-v1 section 11.1, i18n-v1 section 8.1): `direction`
// inherits and reaches Yoga, so one stylesheet written with the logical inline-axis longhands
// mirrors its boxes between LTR and RTL, while the physical longhands stay physical.
//
// The `ar-XB` pseudo-locale case is asserted through the mirrored rectangles: the reduced-ICU data
// pack that generates `ar-XB` is not part of the default test build (tests/unit/icu is registered
// only with TESSERA_ICU_REDUCED_DATA), and the runtime feeds the resolved base direction
// (`LocaleResolution.right_to_left`) into the root's `direction` property. Laying the same sheet
// out with `direction: rtl` is therefore exactly the layout that pseudo-locale produces; the
// resolver's own ar-XB -> ar -> RTL step is covered by the ICU-gated locale_resolver test.

#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/layout/layout_signature.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::ui::layout::layout_signature;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::CompiledStyleSheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Origin;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleNode;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "logical_layout: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

const LayoutBox* find(const LayoutResult& result, std::uint64_t id) {
    for (const LayoutBox& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

bool box_is(const LayoutResult& result, std::uint64_t id, float x, float y, float width,
            float height) {
    const LayoutBox* box = find(result, id);
    const bool matched = box != nullptr && near(box->bounds.x, x) && near(box->bounds.y, y) &&
                         near(box->bounds.width, width) && near(box->bounds.height, height);
    if (!matched && box != nullptr) {
        std::cerr << "  box " << id << " expected " << x << "," << y << " " << width << "x"
                  << height << " got " << box->bounds.x << "," << box->bounds.y << " "
                  << box->bounds.width << "x" << box->bounds.height << "\n";
    }
    return matched;
}

// One compiled sheet, asked for the computed style of a node with a parent. Inheritance is what
// carries `direction` (and every other inherited property) to descendants.
class Sheet {
public:
    explicit Sheet(std::string_view css)
        : compiled_(compile_stylesheet(css, Origin::Author, PropertyRegistry{})) {}

    ComputedStyle style(const ComputedStyle* parent,
                        std::initializer_list<const char*> classes) const {
        StyleNode node;
        node.tag = "div";
        for (const char* name : classes) {
            node.classes.emplace_back(name);
        }
        return compiled_.compute(node, parent);
    }

private:
    CompiledStyleSheet compiled_;
};

// `padding-inline-*` and `margin-inline-start` mirror: the inline start is the left edge in LTR and
// the right edge in RTL, and the content-box insets swap with them.
bool test_inline_padding_and_margin_mirror() {
    constexpr std::string_view kCss = R"(
        .base { box-sizing: border-box; display: flex; width: 300px; height: 60px;
                padding-inline-start: 10px; padding-inline-end: 4px; }
        .ltr { direction: ltr; }
        .rtl { direction: rtl; }
        .item { width: 50px; height: 20px; }
        .first { margin-inline-start: 5px; }
    )";
    const Sheet sheet(kCss);
    const LayoutEngine engine;
    const LayoutViewport viewport{300.0f, 60.0f};
    const auto build = [&sheet](const char* direction_class) {
        LayoutInput root;
        root.id = 1;
        root.style = sheet.style(nullptr, {"base", direction_class});
        const ComputedStyle item = sheet.style(&root.style, {"item"});
        const ComputedStyle first = sheet.style(&root.style, {"item", "first"});
        root.children.push_back(LayoutInput{2, first, {}, {}});
        root.children.push_back(LayoutInput{3, item, {}, {}});
        return root;
    };
    const LayoutResult ltr_result = engine.layout(build("ltr"), viewport);
    const LayoutResult rtl_result = engine.layout(build("rtl"), viewport);
    if (!expect(box_is(ltr_result, 2, 15, 0, 50, 20), "LTR first item at the inline start") ||
        !expect(box_is(ltr_result, 3, 65, 0, 50, 20), "LTR second item follows") ||
        !expect(box_is(rtl_result, 2, 235, 0, 50, 20), "RTL first item at the inline start") ||
        !expect(box_is(rtl_result, 3, 185, 0, 50, 20), "RTL second item follows")) {
        return false;
    }
    const LayoutBox* ltr_root = find(ltr_result, 1);
    const LayoutBox* rtl_root = find(rtl_result, 1);
    return expect(ltr_root != nullptr && near(ltr_root->content_insets.left, 10.0f) &&
                      near(ltr_root->content_insets.right, 4.0f),
                  "LTR inline padding resolves left/right") &&
           expect(rtl_root != nullptr && near(rtl_root->content_insets.left, 4.0f) &&
                      near(rtl_root->content_insets.right, 10.0f),
                  "RTL inline padding resolves right/left");
}

// An absolutely positioned child with only `inset-inline-start` mirrors to the opposite edge.
bool test_inset_inline_start_mirrors() {
    constexpr std::string_view kCss = R"(
        .base { width: 300px; height: 60px; position: relative; }
        .ltr { direction: ltr; }
        .rtl { direction: rtl; }
        .abs { position: absolute; width: 40px; height: 20px; inset-inline-start: 10px; }
    )";
    const Sheet sheet(kCss);
    const LayoutEngine engine;
    const LayoutViewport viewport{300.0f, 60.0f};
    const auto build = [&sheet](const char* direction_class) {
        LayoutInput root;
        root.id = 1;
        root.style = sheet.style(nullptr, {"base", direction_class});
        root.children.push_back(LayoutInput{2, sheet.style(&root.style, {"abs"}), {}, {}});
        return root;
    };
    const LayoutResult ltr_result = engine.layout(build("ltr"), viewport);
    const LayoutResult rtl_result = engine.layout(build("rtl"), viewport);
    return expect(box_is(ltr_result, 2, 10, 0, 40, 20), "LTR inset at the left edge") &&
           expect(box_is(rtl_result, 2, 250, 0, 40, 20), "RTL inset at the right edge");
}

// A logical border reserves layout width on the inline-start side, so children shift with it.
bool test_logical_border_reserves_space() {
    constexpr std::string_view kCss = R"(
        .base { box-sizing: border-box; display: flex; width: 300px; height: 60px;
                border-inline-start-width: 6px; border-inline-start-style: solid;
                border-inline-start-color: red; padding-inline-start: 4px; }
        .ltr { direction: ltr; }
        .rtl { direction: rtl; }
        .item { width: 50px; height: 20px; }
    )";
    const Sheet sheet(kCss);
    const LayoutEngine engine;
    const LayoutViewport viewport{300.0f, 60.0f};
    const auto build = [&sheet](const char* direction_class) {
        LayoutInput root;
        root.id = 1;
        root.style = sheet.style(nullptr, {"base", direction_class});
        root.children.push_back(LayoutInput{2, sheet.style(&root.style, {"item"}), {}, {}});
        return root;
    };
    const LayoutResult ltr_result = engine.layout(build("ltr"), viewport);
    const LayoutResult rtl_result = engine.layout(build("rtl"), viewport);
    const LayoutBox* ltr_root = find(ltr_result, 1);
    const LayoutBox* rtl_root = find(rtl_result, 1);
    return expect(box_is(ltr_result, 2, 10, 0, 50, 20), "LTR border and padding at the left") &&
           expect(box_is(rtl_result, 2, 240, 0, 50, 20), "RTL border and padding at the right") &&
           expect(ltr_root != nullptr && near(ltr_root->content_insets.left, 10.0f) &&
                      near(ltr_root->content_insets.right, 0.0f),
                  "LTR inline border is in the left inset") &&
           expect(rtl_root != nullptr && near(rtl_root->content_insets.left, 0.0f) &&
                      near(rtl_root->content_insets.right, 10.0f),
                  "RTL inline border is in the right inset");
}

// Physical longhands keep meaning the physical side under both directions; only the logical one
// mirrors.
bool test_physical_longhands_do_not_mirror() {
    constexpr std::string_view kCss = R"(
        .base { width: 300px; height: 60px; position: relative; }
        .ltr { direction: ltr; }
        .rtl { direction: rtl; }
        .item { position: absolute; width: 40px; height: 20px; }
        .physical { left: 10px; }
        .logical { inset-inline-start: 10px; }
    )";
    const Sheet sheet(kCss);
    const LayoutEngine engine;
    const LayoutViewport viewport{300.0f, 60.0f};
    const auto layout_child = [&engine, &sheet, &viewport](const char* direction_class,
                                                           const char* child_class) {
        LayoutInput root;
        root.id = 1;
        root.style = sheet.style(nullptr, {"base", direction_class});
        root.children.push_back(
            LayoutInput{2, sheet.style(&root.style, {"item", child_class}), {}, {}});
        return engine.layout(root, viewport);
    };
    const LayoutResult physical_ltr = layout_child("ltr", "physical");
    const LayoutResult physical_rtl = layout_child("rtl", "physical");
    const LayoutResult logical_ltr = layout_child("ltr", "logical");
    const LayoutResult logical_rtl = layout_child("rtl", "logical");
    return expect(box_is(physical_ltr, 2, 10, 0, 40, 20), "LTR physical left") &&
           expect(box_is(physical_rtl, 2, 10, 0, 40, 20), "RTL physical left stays on the left") &&
           expect(box_is(logical_ltr, 2, 10, 0, 40, 20), "LTR logical inset at the left") &&
           expect(box_is(logical_rtl, 2, 250, 0, 40, 20), "RTL logical inset at the right");
}

// The layout cache key must include the logical longhands, or a logical change would reuse a stale
// signature.
bool test_layout_signature_includes_logical_properties() {
    LayoutInput base;
    base.id = 1;
    base.style.set("width", "300px");
    const LayoutViewport viewport{300.0f, 60.0f};
    const std::uint64_t baseline = layout_signature(base, viewport);
    const char* properties[] = {"margin-inline-start", "padding-inline-end", "inset-inline-start",
                                "border-inline-start-width"};
    for (const char* property : properties) {
        LayoutInput changed = base;
        changed.style.set(property, "1px");
        if (!expect(layout_signature(changed, viewport) != baseline, property)) {
            return false;
        }
    }
    return true;
}

} // namespace

int main() {
    bool ok = true;
    ok = test_inline_padding_and_margin_mirror() && ok;
    ok = test_inset_inline_start_mirrors() && ok;
    ok = test_logical_border_reserves_space() && ok;
    ok = test_physical_longhands_do_not_mirror() && ok;
    ok = test_layout_signature_includes_logical_properties() && ok;
    if (!ok) {
        std::cerr << "logical_layout: failed\n";
        return 1;
    }
    std::cout << "logical_layout: ok\n";
    return 0;
}
