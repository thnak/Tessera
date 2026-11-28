// SPDX-License-Identifier: MIT
//
// Click synthesis (issue #6): same-target pointer clicks with the DOM common-ancestor rule, touch
// slop, cancel, keyboard activation, accessibility invoke, and Click bubbling through bindings.

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"

#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::ClickSource;
using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Modifier;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PointerButton;
using tessera::ui::PointerKind;
namespace key = tessera::ui::key;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "click_synthesis: " << message << "\n";
    }
    return condition;
}

struct Counter {
    int clicks = 0;
    ClickSource last_source = ClickSource::Pointer;
    float last_x = 0.0f;
    float last_y = 0.0f;
};

void on_click(void* context, const Event& event) {
    auto* counter = static_cast<Counter*>(context);
    ++counter->clicks;
    counter->last_source = event.click_source;
    counter->last_x = event.x;
    counter->last_y = event.y;
}

struct Scene {
    Node root{1};
    Node* button_a = nullptr;
    Node* label_a = nullptr;
    Node* button_b = nullptr;
    Node* plain = nullptr;
    Counter a;
    Counter b;
    Counter container;
    Counter plain_counter;

    Scene() {
        root.set_bounds({0, 0, 400, 200});
        auto a_node = std::make_unique<Node>(2);
        a_node->set_bounds({10, 10, 80, 40});
        a_node->set_element_kind("Button");
        a_node->set_focusable(true);
        auto label = std::make_unique<Node>(3);
        label->set_bounds({0, 0, 40, 20});
        label->set_element_kind("Text");
        label_a = &a_node->add_child(std::move(label));
        button_a = &root.add_child(std::move(a_node));
        auto b_node = std::make_unique<Node>(4);
        b_node->set_bounds({150, 10, 80, 40});
        b_node->set_element_kind("Button");
        b_node->set_focusable(true);
        button_b = &root.add_child(std::move(b_node));
        auto plain_node = std::make_unique<Node>(5);
        plain_node->set_bounds({10, 100, 80, 40});
        plain_node->set_element_kind("View");
        plain_node->set_focusable(true);
        plain = &root.add_child(std::move(plain_node));
        button_a->bind_event(EventType::Click, &on_click, &a);
        button_b->bind_event(EventType::Click, &on_click, &b);
        root.bind_event(EventType::Click, &on_click, &container);
        plain->bind_event(EventType::Click, &on_click, &plain_counter);
    }
};

InputEvent pointer(InputEventType type, float x, float y, std::uint32_t id = 1,
                   PointerKind kind = PointerKind::Mouse,
                   PointerButton button = PointerButton::Left) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.pointer_id = id;
    event.pointer_kind = kind;
    event.button = button;
    event.is_primary = true;
    return event;
}

InputEvent touch(InputEventType type, float x, float y, std::uint32_t id = 7) {
    return pointer(type, x, y, id, PointerKind::Touch, PointerButton::Left);
}

InputEvent keyboard(InputEventType type, tessera::ui::KeyCode code, bool repeat = false,
                    Modifier modifiers = Modifier::None) {
    InputEvent event;
    event.type = type;
    event.key = code;
    event.repeat = repeat;
    event.modifiers = modifiers;
    return event;
}

bool pointer_clicks() {
    Scene scene;
    InputRouter router;
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 20, 20));
    if (!expect(scene.a.clicks == 1, "press and release on a button clicks it") ||
        !expect(scene.a.last_source == ClickSource::Pointer, "pointer source") ||
        !expect(scene.container.clicks == 1, "click bubbles to the container binding")) {
        return false;
    }

    // The label is the deepest hit; the Button onClick still fires through bubbling.
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 15, 15));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 15, 15));
    if (!expect(scene.a.clicks == 2, "a press on the label clicks the button")) {
        return false;
    }

    // Press on the label, release on the button padding: the common ancestor is the button.
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 15, 15));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 80, 45));
    if (!expect(scene.a.clicks == 3, "label to button padding clicks the button")) {
        return false;
    }

    // Press A, release B: only the shared parent hears it.
    const int a_before = scene.a.clicks;
    const int container_before = scene.container.clicks;
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 160, 20));
    if (!expect(scene.a.clicks == a_before, "press A release B does not click A") ||
        !expect(scene.b.clicks == 0, "press A release B does not click B") ||
        !expect(scene.container.clicks == container_before + 1,
                "press A release B clicks the common parent")) {
        return false;
    }

    // A mouse drag that returns to the same element still clicks (no mouse slop).
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, pointer(InputEventType::PointerMove, 70, 40));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 25, 25));
    if (!expect(scene.a.clicks == a_before + 1, "mouse drag inside the element still clicks")) {
        return false;
    }

    // A release outside the scene never clicks.
    const int container_now = scene.container.clicks;
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 900, 900));
    if (!expect(scene.container.clicks == container_now, "release outside the scene: no click")) {
        return false;
    }

    // The right button is not a click.
    const int a_now = scene.a.clicks;
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20, 1, PointerKind::Mouse,
                                        PointerButton::Right));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 20, 20, 1, PointerKind::Mouse,
                                        PointerButton::Right));
    return expect(scene.a.clicks == a_now, "right button does not click");
}

bool cancel_and_slop() {
    Scene scene;
    InputRouter router;
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, pointer(InputEventType::PointerCancel, 20, 20));
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 20, 20));
    if (!expect(scene.a.clicks == 0, "a cancelled press never clicks")) {
        return false;
    }

    // Window blur drops the press too.
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20));
    InputEvent blur;
    blur.type = InputEventType::Blur;
    router.dispatch(scene.root, blur);
    router.dispatch(scene.root, pointer(InputEventType::PointerUp, 20, 20));
    if (!expect(scene.a.clicks == 0, "window blur cancels the click")) {
        return false;
    }

    // Touch: a pan beyond the slop is not a click, even when it ends on the same node.
    router.dispatch(scene.root, touch(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, touch(InputEventType::PointerMove, 60, 20));
    router.dispatch(scene.root, touch(InputEventType::PointerMove, 22, 20));
    router.dispatch(scene.root, touch(InputEventType::PointerUp, 22, 20));
    if (!expect(scene.a.clicks == 0, "a touch pan beyond the slop does not click")) {
        return false;
    }

    // Touch: a small wobble within the slop still clicks.
    router.dispatch(scene.root, touch(InputEventType::PointerDown, 20, 20));
    router.dispatch(scene.root, touch(InputEventType::PointerMove, 24, 22));
    router.dispatch(scene.root, touch(InputEventType::PointerUp, 24, 22));
    if (!expect(scene.a.clicks == 1, "a touch tap within the slop clicks")) {
        return false;
    }

    // A second finger never clicks: only the primary pointer does.
    router.dispatch(scene.root, pointer(InputEventType::PointerDown, 20, 20, 1));
    router.dispatch(scene.root, touch(InputEventType::PointerDown, 160, 20, 2));
    router.dispatch(scene.root, touch(InputEventType::PointerUp, 160, 20, 2));
    return expect(scene.b.clicks == 0, "a non-primary pointer does not click");
}

bool keyboard_and_invoke() {
    Scene scene;
    InputRouter router;
    router.focus_manager().set_root(scene.root);
    router.focus_manager().request_focus(scene.button_a);

    router.dispatch(scene.root, keyboard(InputEventType::KeyDown, key::kEnter));
    if (!expect(scene.a.clicks == 1, "Enter on a focused button clicks it") ||
        !expect(scene.a.last_source == ClickSource::Keyboard, "keyboard source") ||
        !expect(scene.a.last_x == 40.0f && scene.a.last_y == 20.0f,
                "keyboard click is at the node center, in node-local px (issue #532)")) {
        return false;
    }
    router.dispatch(scene.root, keyboard(InputEventType::KeyDown, key::kEnter, true));
    if (!expect(scene.a.clicks == 1, "an auto-repeated Enter does not click again")) {
        return false;
    }

    router.dispatch(scene.root, keyboard(InputEventType::KeyUp, key::kSpace));
    if (!expect(scene.a.clicks == 1, "Space key-up without its key-down does not click")) {
        return false;
    }
    router.dispatch(scene.root, keyboard(InputEventType::KeyDown, key::kSpace));
    if (!expect(scene.a.clicks == 1, "Space key-down alone does not click")) {
        return false;
    }
    router.dispatch(scene.root, keyboard(InputEventType::KeyUp, key::kSpace));
    if (!expect(scene.a.clicks == 2, "Space key-up after key-down clicks")) {
        return false;
    }

    router.dispatch(scene.root,
                    keyboard(InputEventType::KeyDown, key::kEnter, false, Modifier::Control));
    if (!expect(scene.a.clicks == 2, "Ctrl+Enter is a shortcut, not a click")) {
        return false;
    }

    // Space armed on one node must not fire after focus moved.
    router.dispatch(scene.root, keyboard(InputEventType::KeyDown, key::kSpace));
    router.focus_manager().request_focus(scene.button_b);
    router.dispatch(scene.root, keyboard(InputEventType::KeyUp, key::kSpace));
    if (!expect(scene.b.clicks == 0 && scene.a.clicks == 2,
                "Space armed on another node does not click after a focus move")) {
        return false;
    }

    // A focused non-button never activates.
    router.focus_manager().request_focus(scene.plain);
    router.dispatch(scene.root, keyboard(InputEventType::KeyDown, key::kEnter));
    if (!expect(scene.plain_counter.clicks == 0, "Enter on a non-button does not click")) {
        return false;
    }

    // Accessibility invoke.
    return expect(router.invoke(scene.root, *scene.button_b), "invoke dispatches") &&
           expect(scene.b.clicks == 1 && scene.b.last_source == ClickSource::Accessibility,
                  "invoke clicks with the accessibility source");
}

// `a href` and `label htmlFor` (issue #542): the default action of a click.
struct LinkScene {
    Node root{1};
    Node* link = nullptr;
    Node* checkbox = nullptr;
    Node* field = nullptr;
    Node* label_box = nullptr;
    Node* label_field = nullptr;
    Node* label_missing = nullptr;
    Node* label_self = nullptr;
    Counter checkbox_clicks;
    std::vector<std::string> followed;

    Node& add(NodeId id, float x, float y, std::string_view kind) {
        auto node = std::make_unique<Node>(id);
        node->set_bounds({x, y, 60, 20});
        node->set_element_kind(kind);
        return root.add_child(std::move(node));
    }

    LinkScene() {
        root.set_bounds({0, 0, 400, 200});
        link = &add(10, 10, 10, "Text");
        link->set_role("link");
        link->set_aria("aria-href", "/settings");
        link->set_focusable(true);
        checkbox = &add(11, 10, 50, "View");
        checkbox->set_role("checkbox");
        checkbox->set_element_id("agree");
        checkbox->set_focusable(true);
        checkbox->bind_event(EventType::Click, &on_click, &checkbox_clicks);
        field = &add(12, 10, 90, "TextInput");
        field->set_element_id("name");
        field->set_focusable(true);
        label_box = &add(13, 100, 50, "Text");
        label_box->set_aria("aria-for", "agree");
        label_field = &add(14, 100, 90, "Text");
        label_field->set_aria("aria-for", "name");
        label_missing = &add(15, 100, 130, "Text");
        label_missing->set_aria("aria-for", "nobody");
        label_self = &add(16, 10, 130, "Text");
        label_self->set_element_id("self");
        label_self->set_aria("aria-for", "self");
    }
};

bool click_at(InputRouter& router, Node& root, float x, float y) {
    router.dispatch(root, pointer(InputEventType::PointerDown, x, y));
    router.dispatch(root, pointer(InputEventType::PointerUp, x, y));
    return true;
}

bool links_and_labels() {
    LinkScene scene;
    InputRouter router;
    router.focus_manager().set_root(scene.root);

    // No handler installed: a link click is just a click, never a crash.
    click_at(router, scene.root, 20, 20);
    if (!expect(scene.followed.empty(), "no handler, nothing followed")) {
        return false;
    }

    router.set_link_handler([&scene](Node& link, std::string_view href) {
        scene.followed.emplace_back(href);
        (void)link;
    });
    click_at(router, scene.root, 20, 20);
    if (!expect(scene.followed.size() == 1 && scene.followed[0] == "/settings",
                "a pointer click follows the link's aria-href")) {
        return false;
    }

    router.focus_manager().request_focus(scene.link);
    router.dispatch(scene.root, keyboard(InputEventType::KeyDown, key::kEnter));
    if (!expect(scene.followed.size() == 2, "Enter on a focused link follows it")) {
        return false;
    }
    router.dispatch(scene.root, keyboard(InputEventType::KeyDown, key::kSpace));
    router.dispatch(scene.root, keyboard(InputEventType::KeyUp, key::kSpace));
    if (!expect(scene.followed.size() == 2, "Space does not follow a link")) {
        return false;
    }

    // A label forwards focus and the click to a checkbox, and only focus to a text field.
    click_at(router, scene.root, 110, 60);
    if (!expect(router.focus_manager().focused() == scene.checkbox,
                "a label click focuses its control") ||
        !expect(scene.checkbox_clicks.clicks == 1 &&
                    scene.checkbox_clicks.last_source == ClickSource::Accessibility,
                "and activates a checkbox once")) {
        return false;
    }
    click_at(router, scene.root, 110, 100);
    if (!expect(router.focus_manager().focused() == scene.field, "a label focuses a text field") ||
        !expect(scene.checkbox_clicks.clicks == 1, "without clicking anything else")) {
        return false;
    }

    // Clicking the control itself is not forwarded anywhere, a missing id and a self-reference
    // are no-ops, and nothing loops.
    click_at(router, scene.root, 20, 60);
    if (!expect(scene.checkbox_clicks.clicks == 2, "a direct click on the control is one click")) {
        return false;
    }
    router.focus_manager().request_focus(scene.link);
    click_at(router, scene.root, 110, 140);
    click_at(router, scene.root, 20, 140);
    return expect(router.focus_manager().focused() != scene.checkbox &&
                      router.focus_manager().focused() != scene.field &&
                      scene.checkbox_clicks.clicks == 2,
                  "a missing id and a self-reference change nothing");
}

} // namespace

int main() {
    if (!pointer_clicks()) {
        return 1;
    }
    if (!cancel_and_slop()) {
        return 2;
    }
    if (!keyboard_and_invoke()) {
        return 3;
    }
    if (!links_and_labels()) {
        return 4;
    }
    std::cout << "click_synthesis: ok\n";
    return 0;
}
