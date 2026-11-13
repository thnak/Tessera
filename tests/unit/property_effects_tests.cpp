// SPDX-License-Identifier: MIT
//
// Every registered property has a real effect (issue #213). Each test here sets a property in a
// stylesheet, resolves it through the real cascade, and observes the effect on a real path: a
// layout box, painted output, a hit test, or the accessibility tree. None of them asserts "the
// computed value exists" - that is what the registry tests already do, and it is exactly the
// assertion that let a consumer-less property hide.
//
// The companion check that the per-property status table covers the registry lives in
// `property_status_tests.cpp`, so this file stays one component per concern.
//
// Deterministic: stub text metrics, no clock, no GPU, no fonts.

#include "tessera/ui/accessibility/a11y_tree.hpp"
#include "tessera/ui/layout/layout_engine.hpp"
#include "tessera/ui/paint/paint_pass.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/container_query.hpp"
#include "tessera/ui/style/interaction_style.hpp"
#include "tessera/ui/style/scene_style.hpp"

#include <cmath>
#include <cstddef>
#include <iostream>
#include <memory>
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
using tessera::ui::PaintKind;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::build_a11y_tree;
using tessera::ui::a11y::BuildStats;
using tessera::ui::layout::LayoutBox;
using tessera::ui::layout::LayoutEngine;
using tessera::ui::layout::LayoutInput;
using tessera::ui::layout::LayoutResult;
using tessera::ui::layout::LayoutViewport;
using tessera::ui::paint::paint_scene;
using tessera::ui::paint::PaintSink;
using tessera::ui::paint::PaintStats;
using tessera::ui::paint::RoundedClip;
using tessera::ui::paint::StyleProvider;
using tessera::ui::paint::TextDraw;
using tessera::ui::style::compile_stylesheet;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::ContainerSize;
using tessera::ui::style::SceneStyleTree;
using tessera::ui::style::sync_interaction_style;

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "property_effects: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::abs(lhs - rhs) <= 0.01f;
}

// ---------------------------------------------------------------------------------------------
// Shared fixtures
// ---------------------------------------------------------------------------------------------

// Resolves computed styles through the real cascade for a scene tree, so a stylesheet rule is what
// drives the effect under test.
class SheetStyles final : public StyleProvider {
public:
    SheetStyles(const tessera::ui::style::CompiledStyleSheet& sheet, Node& root)
        : sheet_(&sheet),
          tree_(root) {}

    ComputedStyle compute(const Node& node) override {
        return tree_.compute(*sheet_, node);
    }

    SceneStyleTree& tree() {
        return tree_;
    }

private:
    const tessera::ui::style::CompiledStyleSheet* sheet_;
    SceneStyleTree tree_;
};

// The layout input the host builds: one computed style per node, plus the text payload.
LayoutInput build_layout_input(const Node& node, StyleProvider& styles) {
    LayoutInput input;
    input.id = node.id();
    input.style = styles.compute(node);
    const auto& paint = node.paint_data();
    if (paint && paint->kind == PaintKind::Text) {
        input.text = paint->text;
    }
    for (const auto& child : node.children()) {
        input.children.push_back(build_layout_input(*child, styles));
    }
    return input;
}

const LayoutBox* find_box(const LayoutResult& result, NodeId id) {
    for (const LayoutBox& box : result.boxes) {
        if (box.id == id) {
            return &box;
        }
    }
    return nullptr;
}

class RecordingSink final : public PaintSink {
public:
    void fill_rect(const Rect& rect, const Color& color) override {
        rects.push_back(rect);
        colors.push_back(color);
    }
    void fill_rounded_rect(const Rect& rect, const Color& color, float) override {
        fill_rect(rect, color);
    }
    void push_clip(const RoundedClip&) override {}
    void pop_clip() override {}
    void draw_text(const TextDraw& run) override {
        texts.push_back(std::string(run.text));
    }

    std::vector<Rect> rects;
    std::vector<Color> colors;
    std::vector<std::string> texts;
};

// ---------------------------------------------------------------------------------------------
// `order` (issue #213): a flex item's `order` changes its position in the layout result.
// ---------------------------------------------------------------------------------------------

bool order_reorders_flex_items() {
    const auto sheet = compile_stylesheet(".a { order: 2; width: 10px; height: 10px; }"
                                          ".b { order: 0; width: 10px; height: 10px; }"
                                          ".c { order: 1; width: 10px; height: 10px; }");
    if (!expect(sheet.diagnostics().empty(), "order stylesheet has no diagnostics")) {
        return false;
    }

    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    Node& a = root.add_child(std::make_unique<Node>(2));
    a.set_class_name("a");
    Node& b = root.add_child(std::make_unique<Node>(3));
    b.set_class_name("b");
    Node& c = root.add_child(std::make_unique<Node>(4));
    c.set_class_name("c");

    SheetStyles styles(sheet, root);
    const LayoutEngine engine;
    const LayoutResult result =
        engine.layout(build_layout_input(root, styles), LayoutViewport{100.0f, 20.0f});

    const LayoutBox* box_a = find_box(result, 2);
    const LayoutBox* box_b = find_box(result, 3);
    const LayoutBox* box_c = find_box(result, 4);
    if (!expect(box_a != nullptr && box_b != nullptr && box_c != nullptr,
                "order: every item has a box")) {
        return false;
    }
    // Source order is a, b, c; `order` 0/1/2 puts b first, then c, then a.
    return expect(near(box_b->bounds.x, 0.0f), "order: `order: 0` item is first") &&
           expect(near(box_c->bounds.x, 10.0f), "order: `order: 1` item is second") &&
           expect(near(box_a->bounds.x, 20.0f), "order: `order: 2` item is third");
}

bool order_ties_keep_source_order() {
    // Two items at the same `order` must not swap: the sort is stable, as CSS requires.
    const auto sheet = compile_stylesheet(".first { order: 1; width: 10px; height: 10px; }"
                                          ".second { order: 1; width: 10px; height: 10px; }"
                                          ".early { order: -1; width: 10px; height: 10px; }");
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    Node& first = root.add_child(std::make_unique<Node>(2));
    first.set_class_name("first");
    Node& second = root.add_child(std::make_unique<Node>(3));
    second.set_class_name("second");
    Node& early = root.add_child(std::make_unique<Node>(4));
    early.set_class_name("early");

    SheetStyles styles(sheet, root);
    const LayoutEngine engine;
    const LayoutResult result =
        engine.layout(build_layout_input(root, styles), LayoutViewport{100.0f, 20.0f});

    const LayoutBox* box_early = find_box(result, 4);
    const LayoutBox* box_first = find_box(result, 2);
    const LayoutBox* box_second = find_box(result, 3);
    if (!expect(box_early != nullptr && box_first != nullptr && box_second != nullptr,
                "order ties: every item has a box")) {
        return false;
    }
    return expect(near(box_early->bounds.x, 0.0f), "order ties: negative order sorts first") &&
           expect(near(box_first->bounds.x, 10.0f), "order ties: earlier sibling stays first") &&
           expect(near(box_second->bounds.x, 20.0f), "order ties: later sibling stays second");
}

bool order_does_not_move_boxes_to_the_wrong_node() {
    // The box walk pairs each Yoga node with its `LayoutInput` through the same permutation, so a
    // reordered item must report its own id at its own position. A mismatch here would silently
    // apply one item's box to another node.
    const auto sheet = compile_stylesheet(".wide { order: 1; width: 40px; height: 10px; }"
                                          ".narrow { order: 0; width: 10px; height: 10px; }");
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    Node& wide = root.add_child(std::make_unique<Node>(2));
    wide.set_class_name("wide");
    Node& narrow = root.add_child(std::make_unique<Node>(3));
    narrow.set_class_name("narrow");

    SheetStyles styles(sheet, root);
    const LayoutEngine engine;
    const LayoutResult result =
        engine.layout(build_layout_input(root, styles), LayoutViewport{100.0f, 20.0f});

    const LayoutBox* box_wide = find_box(result, 2);
    const LayoutBox* box_narrow = find_box(result, 3);
    if (!expect(box_wide != nullptr && box_narrow != nullptr, "order pairing: both boxes exist")) {
        return false;
    }
    return expect(near(box_narrow->bounds.x, 0.0f) && near(box_narrow->bounds.width, 10.0f),
                  "order pairing: the narrow item keeps its own width at the first slot") &&
           expect(near(box_wide->bounds.x, 10.0f) && near(box_wide->bounds.width, 40.0f),
                  "order pairing: the wide item keeps its own width at the second slot");
}

// ---------------------------------------------------------------------------------------------
// `visibility` (issue #213): hidden nodes leave paint, hit testing and the a11y tree, but keep
// their layout box.
// ---------------------------------------------------------------------------------------------

bool visibility_hides_paint_and_hit_test() {
    const auto sheet =
        compile_stylesheet(".hidden { visibility: hidden; background-color: #ff0000; }"
                           ".shown { visibility: visible; background-color: #00ff00; }");
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    Node& hidden = root.add_child(std::make_unique<Node>(2));
    hidden.set_class_name("hidden");
    hidden.set_bounds({0.0f, 0.0f, 50.0f, 50.0f});
    Node& shown = root.add_child(std::make_unique<Node>(3));
    shown.set_class_name("shown");
    shown.set_bounds({50.0f, 0.0f, 50.0f, 50.0f});

    SheetStyles styles(sheet, root);
    const auto sync = sync_interaction_style(
        root,
        [](void* context, const Node& node) {
            return static_cast<StyleProvider*>(context)->compute(node);
        },
        &styles);
    if (!expect(sync.changed_visibility == 1, "visibility: one node changed hidden state") ||
        !expect(hidden.style_hidden(), "visibility: hidden node carries the flag") ||
        !expect(!shown.style_hidden(), "visibility: visible node does not")) {
        return false;
    }

    RecordingSink sink;
    const PaintStats stats = paint_scene(root, styles, sink, {});
    // Only the shown child has a background; the hidden subtree is skipped entirely.
    return expect(stats.hidden_subtrees == 1, "visibility: paint skips the hidden subtree") &&
           expect(sink.rects.size() == 1, "visibility: only the visible box is painted") &&
           expect(root.hit_test_deep(25.0f, 25.0f) != &hidden,
                  "visibility: a hidden node is never the hit target") &&
           expect(root.hit_test_deep(75.0f, 25.0f) == &shown,
                  "visibility: a visible sibling is still hit");
}

bool visibility_keeps_the_layout_box() {
    // Unlike `display: none`, `visibility: hidden` keeps the box, so the sibling does not move.
    const auto sheet =
        compile_stylesheet(".hidden { visibility: hidden; width: 20px; height: 10px; }"
                           ".shown { width: 20px; height: 10px; }");
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 20.0f});
    Node& hidden = root.add_child(std::make_unique<Node>(2));
    hidden.set_class_name("hidden");
    Node& shown = root.add_child(std::make_unique<Node>(3));
    shown.set_class_name("shown");

    SheetStyles styles(sheet, root);
    const LayoutEngine engine;
    const LayoutResult result =
        engine.layout(build_layout_input(root, styles), LayoutViewport{100.0f, 20.0f});

    const LayoutBox* box_hidden = find_box(result, 2);
    const LayoutBox* box_shown = find_box(result, 3);
    if (!expect(box_hidden != nullptr && box_shown != nullptr,
                "visibility layout: both boxes exist")) {
        return false;
    }
    return expect(near(box_hidden->bounds.width, 20.0f),
                  "visibility layout: the hidden box keeps its size") &&
           expect(near(box_shown->bounds.x, 20.0f),
                  "visibility layout: the sibling is not pulled into the gap");
}

// The accessibility layer has its own `StyleProvider` interface (it does not depend on the paint
// library), so a paint provider is adapted with a one-line forwarder, exactly as the host does.
class A11yStyles final : public tessera::ui::a11y::StyleProvider {
public:
    // Qualified: inside this class body the unqualified name would find the base class.
    explicit A11yStyles(tessera::ui::paint::StyleProvider& inner)
        : inner_(&inner) {}

    ComputedStyle compute(const Node& node) override {
        return inner_->compute(node);
    }

private:
    tessera::ui::paint::StyleProvider* inner_;
};

bool visibility_hides_from_the_a11y_tree() {
    const auto sheet = compile_stylesheet(".hidden { visibility: hidden; }");
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 100.0f});
    Node& hidden = root.add_child(std::make_unique<Node>(2));
    hidden.set_class_name("hidden");
    hidden.set_bounds({0.0f, 0.0f, 20.0f, 20.0f});
    Node& shown = root.add_child(std::make_unique<Node>(3));
    shown.set_bounds({20.0f, 0.0f, 20.0f, 20.0f});

    SheetStyles styles(sheet, root);
    (void)sync_interaction_style(
        root,
        [](void* context, const Node& node) {
            return static_cast<StyleProvider*>(context)->compute(node);
        },
        &styles);

    A11ySnapshot snapshot;
    A11yStyles a11y_styles(styles);
    const BuildStats stats = build_a11y_tree(root, a11y_styles, snapshot);
    return expect(snapshot.nodes.size() == 2, "visibility a11y: only two nodes are exposed") &&
           expect(stats.skipped_invisible == 1, "visibility a11y: the hidden subtree is counted") &&
           expect(snapshot.nodes[1].id == 3, "visibility a11y: the visible sibling remains");
}

// ---------------------------------------------------------------------------------------------
// `text-overflow` (issue #213): the stylesheet value reaches painted output as a truncation.
// ---------------------------------------------------------------------------------------------

bool text_overflow_truncates_painted_text() {
    const auto sheet =
        compile_stylesheet(".clip { text-overflow: clip; font-size: 16px; }"
                           ".ellipsis { text-overflow: ellipsis; font-size: 16px; }");
    if (!expect(sheet.diagnostics().empty(), "text-overflow stylesheet has no diagnostics")) {
        return false;
    }

    // The same text and box, once per value: only the ellipsis form may emit the U+2026 glyph.
    const auto painted = [&sheet](const char* class_name) {
        Node root(1);
        root.set_bounds({0.0f, 0.0f, 40.0f, 20.0f});
        Node& text = root.add_child(std::make_unique<Node>(2));
        text.set_class_name(class_name);
        text.set_bounds({0.0f, 0.0f, 40.0f, 20.0f});
        PaintData data;
        data.kind = PaintKind::Text;
        data.text = "abcdefghij";
        text.set_paint_data(data);

        SheetStyles styles(sheet, root);
        RecordingSink sink;
        (void)paint_scene(root, styles, sink, {});
        return sink.texts;
    };

    const std::vector<std::string> clipped = painted("clip");
    const std::vector<std::string> ellipsized = painted("ellipsis");
    bool ellipsis_seen = false;
    for (const std::string& run : ellipsized) {
        if (run == "\xE2\x80\xA6") {
            ellipsis_seen = true;
        }
    }
    bool clip_has_ellipsis = false;
    for (const std::string& run : clipped) {
        if (run == "\xE2\x80\xA6") {
            clip_has_ellipsis = true;
        }
    }
    return expect(!ellipsized.empty(), "text-overflow: the ellipsis form paints text") &&
           expect(ellipsis_seen, "text-overflow: ellipsis emits the U+2026 glyph") &&
           expect(!clip_has_ellipsis, "text-overflow: clip never emits the glyph");
}

// ---------------------------------------------------------------------------------------------
// `container-type` / `container-name` (issue #213): a `@container` rule reaches the computed style
// of a descendant, and the name selects which ancestor is the container.
// ---------------------------------------------------------------------------------------------

struct Sizes {
    const Node* container = nullptr;
    ContainerSize size;
};

bool lookup_size(void* context, const Node& node, ContainerSize& out) {
    const auto* sizes = static_cast<const Sizes*>(context);
    if (&node != sizes->container) {
        return false;
    }
    out = sizes->size;
    return true;
}

bool container_type_and_name_select_the_container() {
    const auto sheet = compile_stylesheet(
        ".box { container-type: inline-size; container-name: sidebar; }"
        ".other { container-type: inline-size; container-name: main; }"
        "@container sidebar (min-width: 400px) { .target { background-color: red } }"
        "@container main (min-width: 400px) { .target { background-color: blue } }");

    Node root(1);
    root.set_bounds({0.0f, 0.0f, 800.0f, 200.0f});
    Node& sidebar = root.add_child(std::make_unique<Node>(2));
    sidebar.set_class_name("box");
    Node& main = root.add_child(std::make_unique<Node>(3));
    main.set_class_name("other");
    Node& target = sidebar.add_child(std::make_unique<Node>(4));
    target.set_class_name("target");

    SheetStyles styles(sheet, root);
    // Both containers are wide, so the name is the only thing that can pick one.
    Sizes sizes{&sidebar, ContainerSize{500.0, 100.0, false}};
    const ComputedStyle named = styles.tree().compute(sheet, target, lookup_size, &sizes);
    const std::string* color = named.find("background-color");
    if (!expect(color != nullptr && *color == "red",
                "container: the named ancestor supplies the size")) {
        return false;
    }

    // The same query against the other container's size must not match, proving the name is what
    // selected the container rather than "the nearest one".
    Sizes narrow{&sidebar, ContainerSize{100.0, 100.0, false}};
    const ComputedStyle unmatched = styles.tree().compute(sheet, target, lookup_size, &narrow);
    const std::string* unmatched_color = unmatched.find("background-color");
    return expect(unmatched_color == nullptr || *unmatched_color != "red",
                  "container: a narrow named container does not match");
}

// ---------------------------------------------------------------------------------------------
// `font-style` (issue #213): the computed value reaches the text style the measurer reads.
// ---------------------------------------------------------------------------------------------

bool font_style_reaches_the_text_seam() {
    const auto sheet = compile_stylesheet(".italic { font-style: italic; font-size: 16px; }"
                                          ".normal { font-style: normal; font-size: 16px; }");
    Node root(1);
    root.set_bounds({0.0f, 0.0f, 100.0f, 40.0f});
    Node& italic = root.add_child(std::make_unique<Node>(2));
    italic.set_class_name("italic");
    Node& normal = root.add_child(std::make_unique<Node>(3));
    normal.set_class_name("normal");

    SheetStyles styles(sheet, root);
    const tessera::ui::text::TextStyle italic_style =
        tessera::ui::text::text_style_from(styles.compute(italic));
    const tessera::ui::text::TextStyle normal_style =
        tessera::ui::text::text_style_from(styles.compute(normal));
    return expect(italic_style.italic, "font-style: italic reaches TextStyle::italic") &&
           expect(!normal_style.italic, "font-style: normal does not set italic");
}

} // namespace

int main() {
    bool ok = true;
    ok = order_reorders_flex_items() && ok;
    ok = order_ties_keep_source_order() && ok;
    ok = order_does_not_move_boxes_to_the_wrong_node() && ok;
    ok = visibility_hides_paint_and_hit_test() && ok;
    ok = visibility_keeps_the_layout_box() && ok;
    ok = visibility_hides_from_the_a11y_tree() && ok;
    ok = text_overflow_truncates_painted_text() && ok;
    ok = container_type_and_name_select_the_container() && ok;
    ok = font_style_reaches_the_text_seam() && ok;
    if (!ok) {
        return 1;
    }
    std::cout << "property_effects: ok\n";
    return 0;
}
