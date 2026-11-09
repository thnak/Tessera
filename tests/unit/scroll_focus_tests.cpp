// SPDX-License-Identifier: MIT
//
// Focus-driven scroll-into-view: a successful FocusManager::request_focus (programmatic or the
// InputRouter Tab/Shift+Tab default action) reveals the focus target in every scrollable ancestor,
// innermost to outermost, using each ancestor's scrollport (padding-box insets when set). Already
// visible nodes change no offset and repeating the request is idempotent.

#include "tessera/ui/input.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <cmath>
#include <iostream>
#include <memory>
#include <utility>

namespace {

using tessera::ui::FocusManager;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Modifier;
using tessera::ui::Node;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "scroll_focus: " << message << "\n";
    }
    return condition;
}

bool near(float lhs, float rhs) {
    return std::fabs(lhs - rhs) <= 0.001f;
}

// Root 100x100 with a full-size scrollable container (content 100x400) and two focus targets: one
// near the top and one near the bottom of the scrollable content.
struct FocusScene {
    FocusScene() {
        root.set_bounds({0, 0, 100, 100});
        auto container_owner = std::make_unique<Node>(2);
        container = container_owner.get();
        container->set_bounds({0, 0, 100, 100});
        container->set_scrollable_y(true);
        container->set_content_size(100.0f, 400.0f);
        root.add_child(std::move(container_owner));

        auto above_owner = std::make_unique<Node>(3);
        above = above_owner.get();
        above->set_bounds({0, 10, 20, 20});
        above->set_focusable(true);
        container->add_child(std::move(above_owner));

        auto below_owner = std::make_unique<Node>(4);
        below = below_owner.get();
        below->set_bounds({0, 300, 20, 20});
        below->set_focusable(true);
        container->add_child(std::move(below_owner));
    }

    Node root{1};
    Node* container = nullptr;
    Node* above = nullptr;
    Node* below = nullptr;
};

InputEvent tab_key(bool shift) {
    InputEvent event;
    event.type = InputEventType::KeyDown;
    event.key = tessera::ui::key::kTab;
    if (shift) {
        event.modifiers = Modifier::Shift;
    }
    return event;
}

bool request_focus_scrolls_below_and_is_idempotent() {
    FocusScene scene;
    FocusManager focus;
    focus.set_root(scene.root);

    if (!expect(focus.request_focus(scene.below), "focus request accepted") ||
        !expect(focus.focused() == scene.below, "target focused") ||
        !expect(near(scene.container->scroll_y(), 220.0f),
                "focus scrolls the target below into view")) {
        return false;
    }
    if (!expect(!tessera::ui::scroll_into_view(*scene.below) ||
                    near(scene.container->scroll_y(), 220.0f),
                "already visible after the reveal")) {
        return false;
    }
    // Requesting focus again re-reveals but must not move an already-visible node.
    if (!expect(focus.request_focus(scene.below), "repeat request accepted") ||
        !expect(near(scene.container->scroll_y(), 220.0f),
                "focusing an already-visible node changes no offset")) {
        return false;
    }

    // Scroll the target back out of view, then re-request focus: it reveals again.
    tessera::ui::scroll_to(*scene.container, 0.0f, 0.0f);
    if (!expect(focus.request_focus(scene.below), "third request accepted") ||
        !expect(near(scene.container->scroll_y(), 220.0f),
                "a repeated request re-reveals a scrolled-out focus target")) {
        return false;
    }
    return true;
}

bool request_focus_scrolls_up_and_already_visible() {
    FocusScene scene;
    FocusManager focus;
    focus.set_root(scene.root);

    if (!expect(focus.request_focus(scene.above), "top target accepted") ||
        !expect(near(scene.container->scroll_y(), 0.0f),
                "already-visible target changes no offset")) {
        return false;
    }

    tessera::ui::scroll_to(*scene.container, 0.0f, 300.0f);
    if (!expect(focus.request_focus(scene.above), "top target accepted again") ||
        !expect(near(scene.container->scroll_y(), 10.0f),
                "focus scrolls the target above into view")) {
        return false;
    }
    return true;
}

bool focus_reveal_uses_scrollport_insets() {
    FocusScene scene;
    // Inset scrollport: the visible band is [scroll + 5, scroll + 95].
    scene.container->set_scrollport_insets(5.0f, 5.0f, 5.0f, 5.0f);
    FocusManager focus;
    focus.set_root(scene.root);
    return expect(focus.request_focus(scene.below), "inset focus accepted") &&
           expect(near(scene.container->scroll_y(), 225.0f),
                  "the inset scrollport is used for the reveal");
}

bool nested_ancestors_scroll_inner_then_outer() {
    Node root(1);
    root.set_bounds({0, 0, 200, 200});
    auto outer_owner = std::make_unique<Node>(2);
    Node& outer = root.add_child(std::move(outer_owner));
    outer.set_bounds({0, 0, 200, 200});
    outer.set_scrollable_y(true);
    outer.set_content_size(200.0f, 500.0f);

    auto inner_owner = std::make_unique<Node>(3);
    Node& inner = outer.add_child(std::move(inner_owner));
    inner.set_bounds({0, 250, 100, 100});
    inner.set_scrollable_y(true);
    inner.set_content_size(100.0f, 300.0f);

    auto target_owner = std::make_unique<Node>(4);
    Node& target = inner.add_child(std::move(target_owner));
    target.set_bounds({0, 150, 20, 20});
    target.set_focusable(true);

    FocusManager focus;
    focus.set_root(root);
    if (!expect(focus.request_focus(&target), "nested focus accepted") ||
        !expect(near(inner.scroll_y(), 70.0f), "inner ancestor reveals first") ||
        !expect(near(outer.scroll_y(), 150.0f), "outer ancestor reveals next")) {
        return false;
    }
    // Idempotent across both ancestors.
    return expect(focus.request_focus(&target), "nested repeat accepted") &&
           expect(near(inner.scroll_y(), 70.0f) && near(outer.scroll_y(), 150.0f),
                  "nested reveal is idempotent");
}

bool tab_traversal_scrolls_the_focused_node() {
    FocusScene scene;
    InputRouter router;

    const auto first = router.dispatch(scene.root, tab_key(false));
    if (!expect(first.focus_moved, "first Tab moves focus") ||
        !expect(router.focus_manager().focused() == scene.above,
                "first Tab focuses the top target") ||
        !expect(near(scene.container->scroll_y(), 0.0f), "first target needs no scroll")) {
        return false;
    }

    const auto second = router.dispatch(scene.root, tab_key(false));
    if (!expect(second.focus_moved, "second Tab moves focus") ||
        !expect(router.focus_manager().focused() == scene.below,
                "second Tab focuses the bottom target") ||
        !expect(near(scene.container->scroll_y(), 220.0f), "Tab reveals the focused node")) {
        return false;
    }

    const auto back = router.dispatch(scene.root, tab_key(true));
    return expect(back.focus_moved, "Shift+Tab moves focus back") &&
           expect(router.focus_manager().focused() == scene.above,
                  "Shift+Tab focuses the top target") &&
           expect(near(scene.container->scroll_y(), 10.0f), "Shift+Tab reveals the top target");
}

} // namespace

int main() {
    if (!request_focus_scrolls_below_and_is_idempotent()) {
        return 1;
    }
    if (!request_focus_scrolls_up_and_already_visible()) {
        return 2;
    }
    if (!focus_reveal_uses_scrollport_insets()) {
        return 3;
    }
    if (!nested_ancestors_scroll_inner_then_outer()) {
        return 4;
    }
    if (!tab_traversal_scrolls_the_focused_node()) {
        return 5;
    }

    std::cout << "scroll_focus: ok\n";
    return 0;
}
