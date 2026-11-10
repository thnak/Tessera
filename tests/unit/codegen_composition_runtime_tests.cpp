// SPDX-License-Identifier: MIT

// End-to-end runtime proof of component composition and the static `Children` slot: the generated
// caller builds its own slot content, the generated callee attaches it at its Slot, and every
// instance keeps its own generated struct. No GPU.

#include "Card.hpp"
#include "CardPage.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <string_view>

namespace {

namespace ui = tessera::ui;

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

void check_eq(std::string_view actual, std::string_view expected, std::string_view message) {
    if (actual != expected) {
        std::cerr << "FAIL: " << message << "\n  expected: " << expected
                  << "\n  actual:   " << actual << "\n";
        ++failures;
    }
}

// The slot content of the instance is created by the caller and attached by the callee at its Slot,
// in source order, at the callee's own position.
void test_slot_content_is_attached() {
    demo::gen::CardPage page;
    ui::Node root(1000);
    page.build(root);

    check(root.children().size() == 1, "the page builds one root child");
    ui::Node& view = *root.children()[0];
    check_eq(view.element_kind(), "View", "page root element kind");
    check_eq(view.class_name(), "page", "page root class");
    check(view.children().size() == 1, "the page has one component instance");

    ui::Node& host = *view.children()[0];
    check_eq(host.element_kind(), "Component", "the instance has its generated host node");
    check(host.children().size() == 1, "the instance's own root is attached to the host");

    ui::Node& card = *host.children()[0];
    check_eq(card.element_kind(), "View", "the callee root element kind");
    check_eq(card.class_name(), "card", "the callee root class");
    check(card.children().size() == 4, "callee text plus the three slot children");

    check_eq(card.children()[0]->element_kind(), "Text", "callee text first");
    if (card.children()[0]->paint_data()) {
        check_eq(card.children()[0]->paint_data()->text, "card", "callee text payload");
    } else {
        check(false, "callee text has a paint payload");
    }

    ui::Node& hello = *card.children()[1];
    check_eq(hello.element_kind(), "Text", "slot text is attached at the Slot position");
    if (hello.paint_data()) {
        check_eq(hello.paint_data()->text, "hello", "slot text payload");
    } else {
        check(false, "slot text has a paint payload");
    }

    ui::Node& wrapper = *card.children()[2];
    check_eq(wrapper.element_kind(), "View", "nested slot wrapper");
    check(wrapper.children().size() == 1, "the nested wrapper carries its text");
    if (wrapper.children()[0]->paint_data()) {
        check_eq(wrapper.children()[0]->paint_data()->text, "nested", "nested text payload");
    }

    ui::Node& button = *card.children()[3];
    check_eq(button.element_kind(), "Button", "event-bound slot child");
    check(button.focusable(), "the slot Button is focusable");
    check_eq(button.parent() == &card ? "card" : "other", "card",
             "slot content belongs to the callee tree at run time");
}

// An empty child list is a valid instantiation: the callee renders nothing at its Slot.
void test_empty_slot() {
    demo::gen::Card card;
    ui::Node host(2000);
    card.build(host);
    check(host.children().size() == 1, "the callee root is attached");
    ui::Node& root = *host.children()[0];
    check_eq(root.element_kind(), "View", "callee root element kind");
    check(root.children().size() == 1, "only the callee's own text is present");
    check_eq(root.children()[0]->element_kind(), "Text", "no slot content is invented");
}

// The caller drives the instance lifecycle; the generated calls compile and run.
void test_instance_lifecycle() {
    demo::gen::CardPage page;
    ui::Node root(3000);
    page.build(root);
    page.mount();
    page.update();
    page.unmount();
    check(root.children().size() == 1, "the tree is intact after the lifecycle calls");
}

} // namespace

int main() {
    test_slot_content_is_attached();
    test_empty_slot();
    test_instance_lifecycle();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "codegen composition runtime tests passed\n";
    return 0;
}
