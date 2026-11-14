// SPDX-License-Identifier: MIT
//
// Interaction properties (issue #247, stylesheet-v1 section 11): `pointer-events: none` in scene
// hit testing, `user-select` as the selection gate, and the `@supports` answers registration
// drives. The scene nodes get both values through `style::sync_interaction_style`, exactly as
// `FramePipeline::sync_style` does it in a frame.

#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/style/compiled_stylesheet.hpp"
#include "tessera/ui/style/interaction_style.hpp"
#include "tessera/ui/style/supports.hpp"

#include <iostream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <utility>

namespace {

using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::UserSelect;
using tessera::ui::style::ComputedStyle;
using tessera::ui::style::CursorKind;
using tessera::ui::style::evaluate_supports;
using tessera::ui::style::parse_supports_condition;
using tessera::ui::style::PropertyRegistry;
using tessera::ui::style::StyleNode;
using tessera::ui::style::sync_interaction_style;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "interaction_style: " << message << "\n";
    }
    return condition;
}

// Stub computed styles: only the interaction properties relevant to a node, nothing else.
struct StubStyles {
    struct Values {
        const char* pointer_events = nullptr;
        const char* user_select = nullptr;
        const char* cursor = nullptr;
    };
    std::map<const Node*, Values> nodes;

    void set(const Node& node, const char* pointer_events, const char* user_select,
             const char* cursor = nullptr) {
        nodes[&node] = Values{pointer_events, user_select, cursor};
    }
};

ComputedStyle stub_compute(void* context, const Node& node) {
    StubStyles& styles = *static_cast<StubStyles*>(context);
    ComputedStyle style;
    const auto found = styles.nodes.find(&node);
    if (found == styles.nodes.end()) {
        return style;
    }
    if (found->second.pointer_events != nullptr) {
        style.set("pointer-events", found->second.pointer_events);
    }
    if (found->second.user_select != nullptr) {
        style.set("user-select", found->second.user_select);
    }
    if (found->second.cursor != nullptr) {
        style.set("cursor", found->second.cursor);
    }
    return style;
}

// Records what a node received, so a test can prove an event never reached it.
struct Probe : Node {
    using Node::Node;

    void on_event(Event& event) override {
        if (event.type == EventType::PointerDown) {
            ++downs;
        } else if (event.type == EventType::Click) {
            ++clicks;
        }
    }

    int downs = 0;
    int clicks = 0;
};

Probe& add_probe(Node& parent, NodeId id, tessera::rendering::rhi::Rect bounds) {
    auto child = std::make_unique<Probe>(id);
    child->set_bounds(bounds);
    return static_cast<Probe&>(parent.add_child(std::move(child)));
}

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

bool matches(std::string_view text, const PropertyRegistry& registry) {
    const auto parsed = parse_supports_condition(text);
    return parsed.ok && evaluate_supports(parsed.condition, registry);
}

bool value_is(const ComputedStyle& style, std::string_view property, std::string_view expected) {
    const std::string* value = style.find(property);
    return value != nullptr && *value == expected;
}

// The full chain of one test rule: hit testing, hover/active/click routing and the runtime restyle.
int test_pointer_events() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    Probe& under = add_probe(root, 2, {0, 0, 100, 100});
    Probe& overlay = add_probe(root, 3, {0, 0, 100, 100});

    if (!expect(root.hit_test_deep(50, 50) == &overlay, "the overlay starts on top")) {
        return 1;
    }
    StubStyles styles;
    styles.set(overlay, "none", nullptr);
    if (!expect(sync_interaction_style(root, &stub_compute, &styles).changed_pointer_events == 1,
                "the overlay change is counted") ||
        !expect(!overlay.pointer_events(), "overlay is not hit testable") ||
        !expect(root.hit_test_deep(50, 50) == &under, "the hit goes to the node behind")) {
        return 2;
    }

    // Hover, active and click all route to the node behind; the overlay receives nothing.
    InputRouter router;
    const auto hover = router.dispatch(root, pointer(InputEventType::PointerMove, 50, 50));
    if (!expect(hover.target == &under, "hover target is behind") ||
        !expect(tessera::ui::style::has_state(under.pseudo_state(),
                                              tessera::ui::style::PseudoState::Hover),
                "the behind node hovers") ||
        !expect(!tessera::ui::style::has_state(overlay.pseudo_state(),
                                               tessera::ui::style::PseudoState::Hover),
                "the none overlay never hovers")) {
        return 3;
    }
    router.dispatch(root, pointer(InputEventType::PointerDown, 50, 50));
    router.dispatch(root, pointer(InputEventType::PointerUp, 50, 50));
    if (!expect(under.downs == 1 && overlay.downs == 0, "pointer down reaches the node behind") ||
        !expect(under.clicks == 1 && overlay.clicks == 0, "click reaches the node behind")) {
        return 4;
    }

    // A later stylesheet change updates hit testing without rebuilding anything.
    styles.set(overlay, "auto", nullptr);
    if (!expect(sync_interaction_style(root, &stub_compute, &styles).changed_pointer_events == 1,
                "the restyle is counted") ||
        !expect(root.hit_test_deep(50, 50) == &overlay, "the overlay is hit again")) {
        return 5;
    }
    return 0;
}

// A `none` parent still searches its subtree: an explicit `auto` child is hit again.
int test_descendant_re_enable() {
    Node root(10);
    root.set_bounds({0, 0, 100, 100});
    Probe& panel = add_probe(root, 11, {0, 0, 100, 100});
    Probe& button = add_probe(panel, 12, {10, 10, 40, 20});

    StubStyles styles;
    styles.set(panel, "none", nullptr);
    styles.set(button, "auto", nullptr);
    const auto result = sync_interaction_style(root, &stub_compute, &styles);
    if (!expect(result.changed_pointer_events == 1, "the parent change is counted") ||
        !expect(!panel.pointer_events() && button.pointer_events(), "explicit auto re-enables") ||
        !expect(root.hit_test_deep(20, 20) == &button, "the auto child is hit") ||
        !expect(root.hit_test_deep(80, 80) == &root, "the none panel is skipped")) {
        return 6;
    }
    return 0;
}

// `user-select` policy on the scene plus the inherited/initial resolution through the real cascade.
int test_user_select() {
    Node root(20);
    root.set_bounds({0, 0, 100, 100});
    Probe& label = add_probe(root, 21, {0, 0, 100, 20});
    Probe& field = add_probe(root, 22, {0, 20, 100, 20});
    field.set_element_kind("TextInput");

    StubStyles styles;
    styles.set(label, nullptr, "none");
    styles.set(field, nullptr, "none");
    if (!expect(sync_interaction_style(root, &stub_compute, &styles).changed_user_select == 2,
                "both selection policies are counted") ||
        !expect(!label.text_selection_allowed(), "none blocks starting a selection") ||
        !expect(field.text_selection_allowed(), "an editable field always allows selection") ||
        !expect(field.user_select() == UserSelect::None, "the field still records the value")) {
        return 7;
    }
    styles.set(label, nullptr, "text");
    sync_interaction_style(root, &stub_compute, &styles);
    if (!expect(label.user_select() == UserSelect::Text && label.text_selection_allowed(),
                "text allows selection") ||
        !expect(label.text_selection_allowed(), "text stays selectable")) {
        return 8;
    }
    // `user-select: all` is unsupported in V1: it is reported at parse time and falls back to auto.
    styles.set(label, nullptr, "all");
    sync_interaction_style(root, &stub_compute, &styles);
    if (!expect(label.user_select() == UserSelect::Auto,
                "an unsupported value falls back to auto")) {
        return 9;
    }

    // Inherited resolution through the real registry/cascade, and aspect-ratio staying
    // non-inherited.
    const auto sheet =
        tessera::ui::style::compile_stylesheet(".none { pointer-events: none; user-select: none; }"
                                               ".text { user-select: text; }"
                                               ".ratio { aspect-ratio: 2 / 1; }");
    StyleNode none_view;
    none_view.tag = "div";
    none_view.classes = {"none"};
    const ComputedStyle none_style = sheet.compute(none_view, nullptr);

    StyleNode text_view;
    text_view.tag = "div";
    text_view.classes = {"text"};
    text_view.parent = &none_view;
    const ComputedStyle text_style = sheet.compute(text_view, &none_style);
    if (!expect(none_style.find("user-select") != nullptr, "none parent resolves none") ||
        !expect(value_is(text_style, "user-select", "text"), "text wins on the child") ||
        !expect(value_is(text_style, "pointer-events", "none"), "pointer-events inherits none")) {
        return 10;
    }

    StyleNode plain_view;
    plain_view.tag = "div";
    plain_view.parent = &none_view;
    const ComputedStyle plain_style = sheet.compute(plain_view, &none_style);
    if (!expect(value_is(plain_style, "user-select", "none"), "user-select inherits none")) {
        return 11;
    }

    StyleNode ratio_view;
    ratio_view.tag = "div";
    ratio_view.classes = {"ratio"};
    const ComputedStyle ratio_style = sheet.compute(ratio_view, nullptr);
    StyleNode plain_ratio_view;
    plain_ratio_view.tag = "div";
    plain_ratio_view.parent = &ratio_view;
    const ComputedStyle plain_ratio_style = sheet.compute(plain_ratio_view, &ratio_style);
    if (!expect(value_is(ratio_style, "aspect-ratio", "2 / 1"), "aspect-ratio parses") ||
        !expect(value_is(plain_ratio_style, "aspect-ratio", "auto"),
                "aspect-ratio does not inherit")) {
        return 12;
    }
    return 0;
}

// `@supports` is driven by registration plus the same value grammar the declaration validator uses.
int test_supports() {
    const PropertyRegistry registry;
    if (!expect(matches("(aspect-ratio: 1/1)", registry), "@supports aspect-ratio ratio") ||
        !expect(matches("(aspect-ratio: 16 / 9)", registry), "@supports spaced ratio") ||
        !expect(matches("(aspect-ratio: auto)", registry), "@supports aspect-ratio auto") ||
        !expect(!matches("(aspect-ratio: wide)", registry), "@supports bad ratio") ||
        !expect(matches("(pointer-events: none)", registry), "@supports pointer-events none") ||
        !expect(!matches("(pointer-events: visiblePainted)", registry),
                "@supports SVG-only pointer-events") ||
        !expect(matches("(user-select: text)", registry), "@supports user-select text") ||
        !expect(!matches("(user-select: all)", registry), "@supports user-select all") ||
        !expect(matches("(cursor: pointer)", registry), "@supports cursor pointer") ||
        !expect(!matches("(cursor: url(a.png))", registry), "@supports cursor url()")) {
        return 13;
    }

    const auto gated = tessera::ui::style::compile_stylesheet(
        "@supports (aspect-ratio: 1/1) { .a { width: 10px; } }"
        "@supports (pointer-events: none) { .b { height: 10px; } }"
        "@supports (aspect-ratio: wide) { .c { width: 30px; } }");
    if (!expect(gated.rules().size() == 2, "@supports gates the rules")) {
        return 14;
    }
    return 0;
}

// Unsupported value forms produce exactly one diagnostic per rule + property and stay in the AST.
int test_unsupported_values() {
    const auto compiled = tessera::ui::style::compile_stylesheet(
        ".x { pointer-events: all; user-select: contain; aspect-ratio: wide; }");
    if (!expect(compiled.diagnostics().size() == 3, "one diagnostic per property") ||
        !expect(compiled.rules().size() == 1, "the declarations stay in the AST")) {
        return 15;
    }
    for (const auto& diagnostic : compiled.diagnostics()) {
        if (!expect(diagnostic.kind == tessera::ui::style::StyleDiagnosticKind::UnsupportedValue,
                    "unsupported value kind")) {
            return 16;
        }
    }
    return 0;
}

// `cursor` reaches the scene node (issue #191, window-chrome-v1.md section 8.5): the resolved kind
// is copied onto the node, it is inherited, a later value overrides, the counter follows, and a
// steady scene reports no change on a second sync.
int test_cursor() {
    Node root(30);
    root.set_bounds({0, 0, 100, 100});
    Probe& button = add_probe(root, 31, {0, 0, 100, 20});
    Probe& label = add_probe(button, 32, {0, 0, 50, 20});

    // A fresh scene carries the initial `auto`, which is the platform arrow.
    if (!expect(root.cursor() == CursorKind::Auto, "a fresh node starts at auto") ||
        !expect(label.cursor() == CursorKind::Auto, "a fresh child starts at auto")) {
        return 20;
    }

    StubStyles styles;
    styles.set(button, nullptr, nullptr, "pointer");
    // The label's inherited value is resolved by the cascade upstream, so the stub carries it too -
    // exactly what the real computed style hands over for an inherited property.
    styles.set(label, nullptr, nullptr, "pointer");
    const auto first = sync_interaction_style(root, &stub_compute, &styles);
    if (!expect(first.changed_cursor == 2, "both cursor changes are counted") ||
        !expect(button.cursor() == CursorKind::Pointer, "pointer reaches the node") ||
        !expect(label.cursor() == CursorKind::Pointer, "the child inherits the parent's pointer")) {
        return 21;
    }

    // A second sync of the same scene reports no change: the setters are no-ops when unchanged.
    if (!expect(sync_interaction_style(root, &stub_compute, &styles).changed_cursor == 0,
                "a steady scene reports no cursor change") ||
        !expect(sync_interaction_style(root, &stub_compute, &styles).nodes == 3,
                "the steady sync still walks the visible nodes")) {
        return 22;
    }

    // A later value overrides the inherited one, and the counter sees exactly that node. `none` is
    // CSS `cursor: none`, a shape of its own (the HAL's hidden pointer).
    styles.set(button, nullptr, nullptr, "none");
    styles.set(label, nullptr, nullptr, "text");
    const auto override_result = sync_interaction_style(root, &stub_compute, &styles);
    if (!expect(override_result.changed_cursor == 2, "both overrides are counted") ||
        !expect(button.cursor() == CursorKind::None, "none overrides the inherited pointer") ||
        !expect(label.cursor() == CursorKind::Text, "text overrides the inherited pointer")) {
        return 23;
    }

    // An unsupported form falls back to `auto` instead of reaching a consumer as something it
    // cannot map (the parser already reported it once).
    styles.set(button, nullptr, nullptr, "url(a.png)");
    sync_interaction_style(root, &stub_compute, &styles);
    if (!expect(button.cursor() == CursorKind::Auto,
                "an unsupported cursor value falls back to auto")) {
        return 24;
    }

    // Inherited resolution through the real registry/cascade: a `pointer` parent with no explicit
    // value on the child computes `pointer` there, and an explicit `auto` child is `auto`.
    const auto sheet = tessera::ui::style::compile_stylesheet(".pointer { cursor: pointer; }"
                                                              ".auto { cursor: auto; }");
    StyleNode parent_view;
    parent_view.tag = "div";
    parent_view.classes = {"pointer"};
    const ComputedStyle parent_style = sheet.compute(parent_view, nullptr);

    StyleNode child_view;
    child_view.tag = "div";
    child_view.parent = &parent_view;
    const ComputedStyle child_style = sheet.compute(child_view, &parent_style);

    StyleNode reset_view;
    reset_view.tag = "div";
    reset_view.classes = {"auto"};
    reset_view.parent = &parent_view;
    const ComputedStyle reset_style = sheet.compute(reset_view, &parent_style);

    if (!expect(value_is(parent_style, "cursor", "pointer"), "the parent resolves pointer") ||
        !expect(value_is(child_style, "cursor", "pointer"),
                "cursor inherits through the cascade") ||
        !expect(value_is(reset_style, "cursor", "auto"),
                "an explicit auto child re-enables the default arrow")) {
        return 25;
    }
    return 0;
}

} // namespace

int main() {
    for (int (*test)() : {&test_pointer_events, &test_descendant_re_enable, &test_user_select,
                          &test_supports, &test_unsupported_values, &test_cursor}) {
        const int code = test();
        if (code != 0) {
            return code;
        }
    }
    std::cout << "interaction_style: ok\n";
    return 0;
}
