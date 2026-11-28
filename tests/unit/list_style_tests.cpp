// SPDX-License-Identifier: MIT

// `list-style-type`, `list-style-position` and the `list-style` shorthand (ADR-059 group A, issue
// #541): the value grammar, the cascade (inheritance, shorthand reset, rejected forms, @supports),
// the invalidation domain, the user-agent defaults of `ul`/`ol` and the painted markers.

#include "tessera/ui/animation/property_dirty.hpp"
#include "tessera/ui/paint/paint_cache.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/list_style.hpp"
#include "tessera/ui/style/ua_stylesheet.hpp"

#include <array>
#include <initializer_list>
#include <iostream>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace {

namespace style = tessera::ui::style;
using tessera::rendering::rhi::Color;
using tessera::rendering::rhi::Rect;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::paint::PaintContext;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;

int failures = 0;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "list_style: " << message << "\n";
        ++failures;
    }
    return condition;
}

void test_grammar() {
    using style::ListStyleType;
    expect(style::parse_list_style_type("disc") == ListStyleType::Disc, "disc");
    expect(style::parse_list_style_type(" UPPER-ROMAN ") == ListStyleType::UpperRoman,
           "case and space are ignored");
    expect(style::parse_list_style_type("lower-latin") == ListStyleType::LowerAlpha,
           "lower-latin is lower-alpha");
    expect(!style::parse_list_style_type("georgian"), "georgian is not drawable");
    expect(!style::parse_list_style_type("\"-\""), "a string marker is not drawable");
    expect(!style::parse_list_style_type(""), "empty");
    expect(style::is_supported_list_style_position("outside"), "outside");
    expect(!style::is_supported_list_style_position("inside"), "inside is rejected");

    expect(style::list_marker_text(ListStyleType::Decimal, 12) == "12. ", "decimal");
    expect(style::list_marker_text(ListStyleType::LowerAlpha, 1) == "a. ", "alpha one");
    expect(style::list_marker_text(ListStyleType::LowerAlpha, 26) == "z. ", "alpha 26");
    expect(style::list_marker_text(ListStyleType::LowerAlpha, 27) == "aa. ", "alpha 27");
    expect(style::list_marker_text(ListStyleType::UpperAlpha, 28) == "AB. ", "alpha 28");
    expect(style::list_marker_text(ListStyleType::LowerRoman, 4) == "iv. ", "roman 4");
    expect(style::list_marker_text(ListStyleType::UpperRoman, 1994) == "MCMXCIV. ", "roman 1994");
    expect(style::list_marker_text(ListStyleType::UpperRoman, 4000) == "4000. ",
           "roman above 3999 falls back to decimal");
    expect(style::list_marker_text(ListStyleType::LowerAlpha, 0) == "0. ",
           "alpha zero falls back to decimal");
    expect(style::list_marker_text(ListStyleType::Decimal, -3) == "-3. ", "negative decimal");
    expect(style::list_marker_text(ListStyleType::LowerRoman, -1) == "-1. ", "negative roman");
    expect(style::list_marker_text(ListStyleType::Disc, 1).empty(), "a bullet has no text");
    expect(style::list_marker_text(ListStyleType::None, 1).empty(), "none has no text");
}

struct Tree {
    style::StyleNode root;
    std::vector<std::unique_ptr<style::StyleNode>> nodes;

    style::StyleNode& add(style::StyleNode& parent, std::string_view tag, std::string_view alias) {
        nodes.push_back(std::make_unique<style::StyleNode>());
        style::StyleNode& node = *nodes.back();
        node.tag = tag;
        node.alias_tag = alias;
        node.parent = &parent;
        parent.children.push_back(&node);
        return node;
    }
};

struct Sheets {
    style::PreparedStyleSheet authored;
    std::array<style::StylesheetSource, 2> sources;
    std::unique_ptr<style::CompiledStyleSheet> sheet;

    Sheets(std::string_view author, bool user_agent) {
        authored = style::prepare_stylesheet(author);
        sources = {style::StylesheetSource{"user-agent", &style::ua_prepared_stylesheet(),
                                           style::Origin::UserAgent, false},
                   style::StylesheetSource{"author", &authored, style::Origin::Author, false}};
        const std::span<const style::StylesheetSource> used(sources.data() + (user_agent ? 0 : 1),
                                                            user_agent ? 2 : 1);
        sheet = std::make_unique<style::CompiledStyleSheet>(used);
    }
};

std::string value_of(const style::ComputedStyle& computed, const char* property) {
    const std::string* value = computed.find(property);
    return value == nullptr ? std::string("<unset>") : *value;
}

// The computed style of `node`, resolving its ancestors first so inherited values flow down.
style::ComputedStyle compute(const Sheets& sheets, const style::StyleNode& node) {
    if (node.parent == nullptr) {
        return sheets.sheet->compute(node, nullptr);
    }
    const style::ComputedStyle parent = compute(sheets, *node.parent);
    return sheets.sheet->compute(node, &parent);
}

void test_cascade() {
    {
        const Sheets sheets("Text { list-style: square }", false);
        expect(sheets.authored.diagnostics.empty(), "a valid shorthand has no diagnostic");
        Tree tree;
        tree.root.tag = "Root";
        style::StyleNode& list = tree.add(tree.root, "Text", "");
        style::StyleNode& item = tree.add(list, "Other", "");
        const style::ComputedStyle computed = compute(sheets, list);
        expect(value_of(computed, "list-style-type") == "square", "the shorthand sets the type");
        expect(value_of(computed, "list-style-position") == "outside", "and the position");
        expect(value_of(compute(sheets, item), "list-style-type") == "square",
               "the type is inherited by a child");
    }
    {
        // A shorthand resets the longhand it omits: `outside` alone returns the type to disc.
        const Sheets sheets("Text { list-style-type: square } Text { list-style: outside }", false);
        Tree tree;
        tree.root.tag = "Root";
        style::StyleNode& node = tree.add(tree.root, "Text", "");
        expect(value_of(compute(sheets, node), "list-style-type") == "disc",
               "an omitted type resets to disc");
    }
    {
        const Sheets sheets("Text { list-style: none }", false);
        Tree tree;
        tree.root.tag = "Root";
        style::StyleNode& node = tree.add(tree.root, "Text", "");
        expect(value_of(compute(sheets, node), "list-style-type") == "none", "none");
    }
    {
        Tree tree;
        tree.root.tag = "Root";
        const Sheets bare("", false);
        style::StyleNode& node = tree.add(tree.root, "Text", "");
        expect(value_of(compute(bare, node), "list-style-type") == "none",
               "the initial type is none (no display: list-item)");
        expect(value_of(compute(bare, node), "list-style-position") == "outside",
               "the initial position is outside");
    }
    // Rejected forms: the declaration is dropped and the diagnostic names the reason.
    const char* rejected[] = {
        "Text { list-style-position: inside }", "Text { list-style-type: georgian }",
        "Text { list-style: url(a.png) }",      "Text { list-style: inside }",
        "Text { list-style: disc square }",     "Text { list-style: outside outside }",
        "Text { list-style: bogus }",
    };
    for (const char* source : rejected) {
        const Sheets sheets(source, false);
        expect(!sheets.authored.diagnostics.empty(), std::string("diagnostic for ") + source);
        Tree tree;
        tree.root.tag = "Root";
        style::StyleNode& node = tree.add(tree.root, "Text", "");
        expect(value_of(compute(sheets, node), "list-style-type") == "none",
               std::string("a rejected declaration changes nothing: ") + source);
    }
}

void test_supports() {
    const Sheets sheets(
        "@supports (list-style-type: decimal) { Text { list-style-type: decimal } }\n"
        "@supports (list-style-type: georgian) { Text { list-style-type: square } }\n"
        "@supports (list-style-position: inside) { Text { list-style-position: inside } }\n"
        "@supports (list-style: lower-roman) { View { list-style-type: lower-roman } }\n",
        false);
    Tree tree;
    tree.root.tag = "Root";
    style::StyleNode& text = tree.add(tree.root, "Text", "");
    style::StyleNode& view = tree.add(tree.root, "View", "");
    expect(value_of(compute(sheets, text), "list-style-type") == "decimal",
           "@supports accepts decimal and rejects georgian");
    expect(value_of(compute(sheets, text), "list-style-position") == "outside",
           "@supports rejects inside");
    expect(value_of(compute(sheets, view), "list-style-type") == "lower-roman",
           "@supports understands the shorthand");
}

void test_invalidation_domain() {
    using tessera::ui::Dirty;
    for (const char* property : {"list-style-type", "list-style-position"}) {
        expect(!tessera::ui::animation::is_layout_property(property),
               std::string(property) + " is not a layout property");
        expect(tessera::ui::animation::dirty_for_property(property) ==
                   (Dirty::Paint | Dirty::Render),
               std::string(property) + " dirties paint and render only");
    }
}

void test_user_agent_defaults() {
    const Sheets sheets("", true);
    Tree tree;
    tree.root.tag = "Root";
    style::StyleNode& outer = tree.add(tree.root, "View", "ul");
    style::StyleNode& item = tree.add(outer, "View", "li");
    style::StyleNode& nested = tree.add(item, "View", "ul");
    style::StyleNode& nested_item = tree.add(nested, "View", "li");
    style::StyleNode& deep = tree.add(nested_item, "View", "ul");
    style::StyleNode& ordered = tree.add(tree.root, "View", "ol");
    style::StyleNode& ordered_item = tree.add(ordered, "View", "li");
    style::StyleNode& ordered_nested = tree.add(ordered_item, "View", "ul");
    style::StyleNode& plain = tree.add(tree.root, "View", "div");
    expect(value_of(compute(sheets, outer), "list-style-type") == "disc", "ul is a disc list");
    expect(value_of(compute(sheets, item), "list-style-type") == "disc", "li inherits the disc");
    expect(value_of(compute(sheets, nested), "list-style-type") == "circle", "ul in ul is circle");
    expect(value_of(compute(sheets, deep), "list-style-type") == "square",
           "ul in ul in ul is square");
    expect(value_of(compute(sheets, ordered), "list-style-type") == "decimal", "ol is decimal");
    expect(value_of(compute(sheets, ordered_item), "list-style-type") == "decimal",
           "an ol item inherits decimal");
    expect(value_of(compute(sheets, ordered_nested), "list-style-type") == "circle",
           "ul in ol is circle");
    expect(value_of(compute(sheets, plain), "list-style-type") == "none", "a div has no marker");
}

// --- paint ---

class Styles final : public StyleProvider {
public:
    void set(NodeId id, std::initializer_list<std::pair<const char*, const char*>> values) {
        style::ComputedStyle computed;
        for (const auto& [property, value] : values) {
            computed.set(property, value);
        }
        styles_[id] = std::move(computed);
    }
    style::ComputedStyle compute(const Node& node) override {
        const auto found = styles_.find(node.id());
        return found == styles_.end() ? style::ComputedStyle{} : found->second;
    }

private:
    std::map<NodeId, style::ComputedStyle> styles_;
};

class Recorder final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        shapes.emplace_back('s', rect, color);
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        shapes.emplace_back('d', rect, color);
    }
    void fill_rounded_ring(const Rect& rect, const Color& color, float, float) override {
        shapes.emplace_back('c', rect, color);
    }
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.emplace_back(std::string(run.text), run.rect);
    }
    std::vector<std::tuple<char, Rect, Color>> shapes;
    std::vector<std::pair<std::string, Rect>> texts;
};

// A list View (200x100 at 0,0) holding three items of 20px; the second one is not a list item when
// `middle_role` is empty.
struct List {
    std::unique_ptr<Node> root;
    Styles styles;
};

List make_list(const char* type, const char* middle_role = "listitem") {
    List list;
    list.root = std::make_unique<Node>(1);
    list.root->set_bounds({0, 0, 200, 100});
    for (NodeId id = 2; id <= 4; ++id) {
        auto item = std::make_unique<Node>(id);
        item->set_bounds({40, static_cast<float>((id - 2) * 20), 160, 20});
        item->set_role(id == 3 ? middle_role : "listitem");
        list.root->add_child(std::move(item));
        list.styles.set(id, {{"font-size", "16"}, {"color", "#000000"}, {"list-style-type", type}});
    }
    list.styles.set(1, {{"width", "200"}, {"height", "100"}});
    return list;
}

void test_paint_markers() {
    {
        List list = make_list("disc");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.root, list.styles, sink, PaintContext{});
        expect(sink.shapes.size() == 3, "three discs");
        if (sink.shapes.size() == 3) {
            const auto& [kind, rect, color] = sink.shapes[0];
            expect(kind == 'd', "a disc is a rounded rect");
            expect(rect.x + rect.width <= 40.0f, "the marker is outside the item box");
            expect(rect.width == 5.0f && rect.height == 5.0f, "the disc is 5px at 16px");
            expect(color.a == 1.0f && color.r == 0.0f, "in the item colour");
            expect(std::get<1>(sink.shapes[1]).y > rect.y, "markers follow their items");
        }
    }
    {
        List list = make_list("circle");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.root, list.styles, sink, PaintContext{});
        expect(sink.shapes.size() == 3 && std::get<0>(sink.shapes[0]) == 'c', "rings");
    }
    {
        List list = make_list("square");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.root, list.styles, sink, PaintContext{});
        expect(sink.shapes.size() == 3 && std::get<0>(sink.shapes[0]) == 's', "squares");
    }
    {
        List list = make_list("decimal");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.root, list.styles, sink, PaintContext{});
        expect(sink.texts.size() == 3, "three counters");
        if (sink.texts.size() == 3) {
            expect(sink.texts[0].first == "1. " && sink.texts[1].first == "2. " &&
                       sink.texts[2].first == "3. ",
                   "counted in order");
            expect(sink.texts[0].second.x + sink.texts[0].second.width == 40.0f,
                   "the counter ends at the item edge");
        }
    }
    {
        // A sibling that is not a list item takes no number and is not counted.
        List list = make_list("decimal", "");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.root, list.styles, sink, PaintContext{});
        expect(sink.texts.size() == 2 && sink.texts[0].first == "1. " &&
                   sink.texts[1].first == "2. ",
               "only list items are counted");
    }
    {
        List list = make_list("none");
        Recorder sink;
        tessera::ui::paint::paint_scene(*list.root, list.styles, sink, PaintContext{});
        expect(sink.shapes.empty() && sink.texts.empty(), "none paints nothing");
    }
    {
        // The key of the paint cache changes with the position, so an inserted sibling repaints.
        tessera::ui::paint::PaintCacheSignature first;
        tessera::ui::paint::PaintCacheSignature second;
        first.list_ordinal = 1;
        second.list_ordinal = 2;
        expect(!tessera::ui::paint::same_paint_signature(first, second),
               "the list position is part of the cache key");
        second.list_ordinal = 1;
        expect(tessera::ui::paint::same_paint_signature(first, second), "and equal when equal");
    }
}

// `ol start`, `reversed` and `li value` arrive as aria-start, aria-reversed and aria-value.
std::vector<std::string>
numbered(const char* type, std::initializer_list<std::pair<const char*, const char*>> list_attrs,
         std::initializer_list<std::pair<int, const char*>> item_values = {}) {
    List list = make_list(type);
    for (const auto& [name, value] : list_attrs) {
        list.root->set_aria(name, value);
    }
    for (const auto& [index, value] : item_values) {
        list.root->children()[static_cast<std::size_t>(index)]->set_aria("aria-value", value);
    }
    Recorder sink;
    tessera::ui::paint::paint_scene(*list.root, list.styles, sink, PaintContext{});
    std::vector<std::string> out;
    for (const auto& text : sink.texts) {
        out.push_back(text.first);
    }
    return out;
}

void test_numbering_attributes() {
    using V = std::vector<std::string>;
    expect(numbered("decimal", {{"aria-start", "5"}}) == V{"5. ", "6. ", "7. "}, "start");
    expect(numbered("decimal", {{"aria-reversed", "true"}}) == V{"3. ", "2. ", "1. "},
           "reversed counts down from the item count");
    expect(numbered("decimal", {{"aria-reversed", "true"}, {"aria-start", "10"}}) ==
               V{"10. ", "9. ", "8. "},
           "reversed with start");
    expect(numbered("decimal", {{"aria-reversed", "false"}}) == V{"1. ", "2. ", "3. "},
           "reversed=false is not reversed");
    expect(numbered("decimal", {{"aria-start", "0"}}) == V{"0. ", "1. ", "2. "}, "start zero");
    expect(numbered("decimal", {{"aria-start", "-1"}}) == V{"-1. ", "0. ", "1. "},
           "negative start");
    expect(numbered("lower-alpha", {{"aria-start", "0"}}) == V{"0. ", "a. ", "b. "},
           "alphabetic below one is decimal");
    expect(numbered("decimal", {{"aria-start", "x"}}) == V{"1. ", "2. ", "3. "},
           "an invalid start is ignored");
    expect(numbered("decimal", {}, {{1, "10"}}) == V{"1. ", "10. ", "11. "},
           "an item value resets the counter for the rest");
    expect(numbered("decimal", {{"aria-reversed", "true"}}, {{1, "7"}}) == V{"3. ", "7. ", "6. "},
           "a value inside a reversed list counts down from it");
    {
        tessera::ui::paint::PaintCacheSignature first;
        tessera::ui::paint::PaintCacheSignature second;
        first.list_item = true;
        second.list_item = false;
        expect(!tessera::ui::paint::same_paint_signature(first, second),
               "an item and a non-item differ even at number 0");
    }
}

} // namespace

int main() {
    test_grammar();
    test_cascade();
    test_supports();
    test_invalidation_domain();
    test_user_agent_defaults();
    test_paint_markers();
    test_numbering_attributes();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    return 0;
}
