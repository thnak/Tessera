// SPDX-License-Identifier: MIT
//
// Logical (direction-relative) properties: registry presence and shorthand expansion, the
// UnsupportedProperty/InvalidShorthand diagnostic discipline, `@supports` agreement, and the paint
// resolution of `border-inline-*` and `text-align: start/end` against `direction` (stylesheet-v1
// section 11.1, issue #230).

#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/property_registry.hpp"
#include "tessera/ui/style/supports.hpp"

#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PaintData;
using tessera::ui::TextDecoration;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::Declaration;
using tessera::ui::style::evaluate_supports;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyExpansionResult;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleDiagnosticKind;
using tessera::ui::style::StyleNode;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "logical_properties: " << message << "\n";
    }
    return condition;
}

ComputedStyle styled(std::initializer_list<std::pair<const char*, const char*>> values) {
    ComputedStyle style;
    for (const auto& [property, value] : values) {
        style.set(property, value);
    }
    return style;
}

const Declaration* find_declaration(const PropertyExpansionResult& result,
                                    std::string_view property) {
    for (const Declaration& declaration : result.declarations) {
        if (declaration.property == property) {
            return &declaration;
        }
    }
    return nullptr;
}

bool decl_is(const PropertyExpansionResult& result, std::string_view property,
             std::string_view value) {
    const Declaration* declaration = find_declaration(result, property);
    return declaration != nullptr && declaration->value == value;
}

bool supported_value(std::string_view text, const PropertyRegistry& registry) {
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, registry);
}

class StubStyles final : public StyleProvider {
public:
    void set(NodeId id, ComputedStyle style) {
        styles_[id] = std::move(style);
    }
    ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, ComputedStyle> styles_;
};

struct Event {
    bool text = false;
    Rect rect;
    Color color;
};

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        events.push_back(Event{false, rect, color});
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        events.push_back(Event{true, run.rect, run.color});
    }

    std::vector<Event> events;
};

bool same_rect(const Rect& actual, const Rect& expected) {
    return std::abs(actual.x - expected.x) <= 0.01f && std::abs(actual.y - expected.y) <= 0.01f &&
           std::abs(actual.width - expected.width) <= 0.01f &&
           std::abs(actual.height - expected.height) <= 0.01f;
}

bool test_registry_presence() {
    const PropertyRegistry registry;
    constexpr const char* kLonghands[] = {
        "margin-inline-start",       "margin-inline-end",         "padding-inline-start",
        "padding-inline-end",        "inset-inline-start",        "inset-inline-end",
        "border-inline-start-width", "border-inline-start-style", "border-inline-start-color",
        "border-inline-end-width",   "border-inline-end-style",   "border-inline-end-color",
    };
    for (const char* name : kLonghands) {
        const auto* info = registry.find(name);
        if (!expect(info != nullptr, name) || !expect(!info->inherited, name) ||
            // Empty initial: an absent logical longhand is an absent inline-axis override, not a
            // stored zero that would shadow the physical property (stylesheet-v1 section 11.1).
            !expect(info->initial.empty(), name) || !expect(registry.supported(name), name)) {
            return false;
        }
    }
    struct ShorthandProbe {
        const char* name;
        std::size_t longhands;
    };
    constexpr ShorthandProbe kShorthands[] = {
        {"margin-inline", 2},       {"padding-inline", 2},      {"inset-inline", 2},
        {"border-inline-width", 2}, {"border-inline-style", 2}, {"border-inline-color", 2},
        {"border-inline-start", 3}, {"border-inline-end", 3},
    };
    for (const ShorthandProbe& probe : kShorthands) {
        const auto* longhands = registry.longhands(probe.name);
        if (!expect(longhands != nullptr && longhands->size() == probe.longhands, probe.name) ||
            !expect(registry.supported(probe.name), probe.name)) {
            return false;
        }
    }
    return true;
}

bool test_expansion() {
    const PropertyRegistry registry;

    PropertyExpansionResult result =
        registry.expand(Declaration{"margin-inline", "10px", false, 3});
    if (!expect(result.diagnostics.empty(), "margin-inline clean") ||
        !expect(decl_is(result, "margin-inline-start", "10px"), "margin-inline start") ||
        !expect(decl_is(result, "margin-inline-end", "10px"), "margin-inline end") ||
        !expect(result.declarations[0].offset == 3, "margin-inline offset")) {
        return false;
    }

    result = registry.expand(Declaration{"padding-inline", "1px 2px", false, 0});
    if (!expect(decl_is(result, "padding-inline-start", "1px"), "padding-inline start") ||
        !expect(decl_is(result, "padding-inline-end", "2px"), "padding-inline end")) {
        return false;
    }

    result = registry.expand(Declaration{"inset-inline", "auto 4px", false, 0});
    if (!expect(decl_is(result, "inset-inline-start", "auto"), "inset-inline start") ||
        !expect(decl_is(result, "inset-inline-end", "4px"), "inset-inline end")) {
        return false;
    }

    result = registry.expand(Declaration{"border-inline-width", "1px 3px", false, 0});
    if (!expect(decl_is(result, "border-inline-start-width", "1px"), "border-inline-width start") ||
        !expect(decl_is(result, "border-inline-end-width", "3px"), "border-inline-width end")) {
        return false;
    }

    result = registry.expand(Declaration{"border-inline-start", "solid", false, 0});
    if (!expect(result.diagnostics.empty(), "border-inline-start shorthand clean") ||
        !expect(decl_is(result, "border-inline-start-width", "0"), "omitted width resets") ||
        !expect(decl_is(result, "border-inline-start-style", "solid"), "style") ||
        !expect(decl_is(result, "border-inline-start-color", "currentColor"),
                "omitted color resets")) {
        return false;
    }

    // The width/style/color grammar is order-independent, as in CSS.
    result = registry.expand(Declaration{"border-inline-end", "red 2px dashed", false, 0});
    if (!expect(result.diagnostics.empty(), "border-inline-end shorthand clean") ||
        !expect(decl_is(result, "border-inline-end-width", "2px"), "width from any position") ||
        !expect(decl_is(result, "border-inline-end-style", "dashed"), "style from any position") ||
        !expect(decl_is(result, "border-inline-end-color", "red"), "color from any position")) {
        return false;
    }
    return true;
}

bool test_invalid_values_are_diagnosed() {
    const PropertyRegistry registry;

    // An invalid shorthand drops the whole declaration: no longhand wins the cascade, and exactly
    // one diagnostic names the property (the existing InvalidShorthand discipline).
    const Declaration too_many{"margin-inline", "1px 2px 3px", false, 9};
    PropertyExpansionResult result = registry.expand(too_many);
    if (!expect(result.declarations.empty(), "invalid margin-inline dropped") ||
        !expect(result.diagnostics.size() == 1, "invalid margin-inline diagnostic") ||
        !expect(result.diagnostics[0].kind == StyleDiagnosticKind::InvalidShorthand,
                "invalid margin-inline kind") ||
        !expect(result.diagnostics[0].offset == 9 &&
                    result.diagnostics[0].property == "margin-inline",
                "invalid margin-inline metadata")) {
        return false;
    }

    result = registry.expand(Declaration{"border-inline-start", "1px solid red blue", false, 0});
    if (!expect(result.declarations.empty() && result.diagnostics.size() == 1,
                "four border side components dropped")) {
        return false;
    }
    result = registry.expand(Declaration{"border-inline-start", "solid dashed", false, 0});
    if (!expect(result.declarations.empty() && result.diagnostics.size() == 1,
                "two styles dropped")) {
        return false;
    }
    result = registry.expand(Declaration{"border-inline-start", "1px 2px", false, 0});
    if (!expect(result.declarations.empty() && result.diagnostics.size() == 1,
                "two widths dropped")) {
        return false;
    }
    result = registry.expand(Declaration{"border-inline-end", "wobbly", false, 0});
    if (!expect(result.declarations.empty() && result.diagnostics.size() == 1,
                "unknown token dropped")) {
        return false;
    }

    // Logical properties V1 does not implement are unsupported properties, not silent drops.
    result = registry.expand(Declaration{"margin-block-start", "1px", false, 0});
    if (!expect(result.declarations.empty() && result.diagnostics.size() == 1 &&
                    result.diagnostics[0].kind == StyleDiagnosticKind::UnsupportedProperty,
                "margin-block-* is an unsupported property")) {
        return false;
    }

    // The diagnostics discipline holds through the compiled stylesheet pipeline too.
    const auto compiled =
        compile_stylesheet(".a { margin-inline: 1px 2px 3px; margin-block-end: 4px; }");
    if (!expect(compiled.diagnostics().size() == 2, "two compiled diagnostics") ||
        !expect(compiled.diagnostics()[0].kind == StyleDiagnosticKind::InvalidShorthand,
                "compiled invalid shorthand")) {
        return false;
    }
    return true;
}

bool test_supports_agrees() {
    const PropertyRegistry registry;
    return expect(supported_value("(margin-inline-start: 10px)", registry),
                  "@supports accepts a logical longhand") &&
           expect(supported_value("(margin-inline: 1px 2px)", registry),
                  "@supports accepts a logical shorthand") &&
           expect(!supported_value("(margin-block-start: 1px)", registry),
                  "@supports rejects an unimplemented logical property") &&
           expect(supported_value("(text-align: start)", registry),
                  "@supports accepts text-align start");
}

bool test_computed_style() {
    const auto sheet = compile_stylesheet(
        ".app { direction: rtl; margin-inline-start: 5px; } .item { width: 1px; }");
    StyleNode root;
    root.tag = "div";
    root.classes = {"app"};
    StyleNode child;
    child.tag = "div";
    child.classes = {"item"};
    const PropertyRegistry registry;
    const ComputedStyle root_style = sheet.compute(root, nullptr);
    const ComputedStyle child_style = sheet.compute(child, &root_style);
    const std::string* direction = child_style.find("direction");
    return expect(direction != nullptr && *direction == "rtl", "direction inherits") &&
           // An undeclared logical longhand is absent, so the physical side stays in charge. The
           // physical longhands keep their stored initial (`margin-left: 0`), which is exactly why
           // a logical longhand cannot carry one.
           expect(child_style.find("margin-inline-start") == nullptr,
                  "undeclared logical longhand stays absent") &&
           expect(registry.initial_style().find("margin-inline-start") == nullptr,
                  "no stored initial value");
}

bool test_border_paint_mirrors() {
    const auto paint_border = [](const char* direction) {
        Node node(1);
        node.set_bounds({0, 0, 100, 40});
        StubStyles styles;
        styles.set(1, styled({{"direction", direction},
                              {"border-inline-start-width", "4px"},
                              {"border-inline-start-style", "solid"},
                              {"border-inline-start-color", "red"}}));
        RecordingSink sink;
        paint_scene(node, styles, sink);
        return sink.events;
    };
    const std::vector<Event> ltr = paint_border("ltr");
    const std::vector<Event> rtl = paint_border("rtl");
    const Color red{1.0f, 0.0f, 0.0f, 1.0f};
    return expect(ltr.size() == 1, "one LTR border fill") &&
           expect(rtl.size() == 1, "one RTL border fill") &&
           expect(same_rect(ltr[0].rect, Rect{0, 0, 4, 40}),
                  "inline-start is the left edge in LTR") &&
           expect(same_rect(rtl[0].rect, Rect{96, 0, 4, 40}),
                  "inline-start is the right edge in RTL") &&
           expect(std::abs(ltr[0].color.r - red.r) <= 0.01f &&
                      std::abs(ltr[0].color.g - red.g) <= 0.01f,
                  "the logical border color reaches paint");
}

bool test_physical_border_does_not_mirror() {
    const auto paint_physical = [](const char* direction) {
        Node node(1);
        node.set_bounds({0, 0, 100, 40});
        StubStyles styles;
        styles.set(1, styled({{"direction", direction},
                              {"border-left-width", "4px"},
                              {"border-left-style", "solid"},
                              {"border-left-color", "red"}}));
        RecordingSink sink;
        paint_scene(node, styles, sink);
        return sink.events;
    };
    const std::vector<Event> ltr = paint_physical("ltr");
    const std::vector<Event> rtl = paint_physical("rtl");
    return expect(ltr.size() == 1 && rtl.size() == 1, "one physical border fill each") &&
           expect(same_rect(ltr[0].rect, Rect{0, 0, 4, 40}), "physical left in LTR") &&
           expect(same_rect(rtl[0].rect, Rect{0, 0, 4, 40}), "physical left stays physical in RTL");
}

bool same_x_width(const Rect& actual, float x, float width) {
    return std::abs(actual.x - x) <= 0.01f && std::abs(actual.width - width) <= 0.01f;
}

bool test_text_align_mirrors() {
    // Realign one 40 px text run inside a 200 px content box: `start` is the inline start, so it
    // flips between the left (LTR) and right (RTL) edge while `left`/`right` stay physical.
    const auto paint_text = [](const char* direction, const char* align) {
        Node node(1);
        node.set_bounds({10, 20, 200, 40});
        PaintData data;
        data.text = "abcde";
        // The decoration path lays out aligned line boxes; the plain fast path paints the whole
        // content box as one run and therefore does not expose alignment.
        data.decoration = TextDecoration{};
        data.decoration->caret_byte_offset = 5;
        node.set_paint_data(std::move(data));
        StubStyles styles;
        styles.set(1, styled({{"direction", direction},
                              {"text-align", align},
                              {"font-size", "16px"},
                              {"color", "white"}}));
        RecordingSink sink;
        paint_scene(node, styles, sink);
        for (const Event& event : sink.events) {
            if (event.text) {
                return event.rect;
            }
        }
        return Rect{};
    };
    const Rect ltr_start = paint_text("ltr", "start");
    const Rect ltr_end = paint_text("ltr", "end");
    const Rect rtl_start = paint_text("rtl", "start");
    const Rect rtl_end = paint_text("rtl", "end");
    const Rect ltr_left = paint_text("ltr", "left");
    const Rect rtl_left = paint_text("rtl", "left");
    return expect(same_x_width(ltr_start, 10.0f, 40.0f), "LTR start is left aligned") &&
           expect(same_x_width(ltr_end, 170.0f, 40.0f), "LTR end is right aligned") &&
           expect(same_x_width(rtl_start, 170.0f, 40.0f), "RTL start is the right edge") &&
           expect(same_x_width(rtl_end, 10.0f, 40.0f), "RTL end is the left edge") &&
           expect(same_x_width(ltr_left, 10.0f, 40.0f), "left stays physical in LTR") &&
           expect(same_x_width(rtl_left, 10.0f, 40.0f), "left stays physical in RTL");
}

} // namespace

int main() {
    bool ok = true;
    ok = test_registry_presence() && ok;
    ok = test_expansion() && ok;
    ok = test_invalid_values_are_diagnosed() && ok;
    ok = test_supports_agrees() && ok;
    ok = test_computed_style() && ok;
    ok = test_border_paint_mirrors() && ok;
    ok = test_physical_border_does_not_mirror() && ok;
    ok = test_text_align_mirrors() && ok;
    if (!ok) {
        std::cerr << "logical_properties: failed\n";
        return 1;
    }
    std::cout << "logical_properties: ok\n";
    return 0;
}
