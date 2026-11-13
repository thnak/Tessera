// SPDX-License-Identifier: MIT
//
// Inline-span accessibility (issue #229): each span is a child entry of the parent Text entry with
// its own slice as the name and its role (the `a` alias injects `link`, which exposes Press). The
// span's bounds are the run rectangle the paint pass stored on the child node.

#include "a11y_test_helpers.hpp"

namespace a11y_test {
namespace {

bool span_entry() {
    Node root(1);
    root.set_bounds({0, 0, 300, 100});
    Node& text = child(root, 2);
    text.set_element_kind("Text");
    text.set_bounds({10, 20, 280, 20});
    tessera::ui::PaintData payload = text_payload("By continuing you agree to the Terms");
    payload.spans = {tessera::ui::PaintSpan{3, 31, 36}};
    text.set_paint_data(std::move(payload));
    Node& link = child(text, 3);
    link.set_element_kind("Text");
    link.set_role("link");
    link.set_paint_data(text_payload("Terms"));
    // The paint pass derives this rectangle; the accessibility snapshot only reads it.
    link.set_span_bounds({248, 0, 40, 20});

    StubStyles styles;
    styles.set(1, styled({{"width", "300"}, {"height", "100"}}));
    styles.set(2, styled({{"font-size", "16"}}));
    styles.set(3, styled({{"font-size", "16"}}));
    A11ySnapshot snapshot;
    const BuildStats stats = build(root, styles, snapshot);
    if (!expect(stats.emitted == 3, "root, Text and span entries are emitted")) {
        return false;
    }
    const A11yNode& parent = snapshot.nodes[1];
    const A11yNode& span = snapshot.nodes[2];
    return expect(parent.role == Role::Text, "the parent keeps the text role") &&
           expect(name_of(parent) == "By continuing you agree to the Terms",
                  "the parent name is the concatenated text") &&
           expect(parent.child_count == 1, "the span is a child of the Text entry") &&
           expect(span.role == Role::Link, "the span exposes the link role") &&
           expect(name_of(span) == "Terms", "the span name is its run slice") &&
           expect((span.actions & Action::Press) != Action::None, "the link exposes Press") &&
           expect(span.bounds.x == 258.0f && span.bounds.width == 40.0f,
                  "the span bounds are the painted run rectangle");
}

} // namespace

bool span_tests() {
    return span_entry();
}

} // namespace a11y_test

int main() {
    return a11y_test::span_tests() ? 0 : 1;
}
