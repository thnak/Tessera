// SPDX-License-Identifier: MIT

// Headless interaction test of the library Switch, Checkbox and Radio (issue #240, ADR-047 item
// 10), through the generated `ControlsPage` and the real `InputRouter`: keyboard and pointer reach
// the caller's `onToggle` / `onSelect`, a disabled control never does, and the accessibility
// contract (role, `aria-checked`, `aria-disabled`) follows the caller's state. No GPU, no clock.

#include "ControlsPage.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"
#include "tessera/ui/scene/node.hpp"
#include "tessera/ui/scene/update_scheduler.hpp"

#include <algorithm>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace ui = tessera::ui;
namespace key = tessera::ui::key;
namespace rhi = tessera::rendering::rhi;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << "\n";
        ++failures;
    }
}

bool is_control(const ui::Node& node) {
    const std::string_view role = node.role();
    return node.focusable() && (role == "switch" || role == "checkbox" || role == "radio");
}

void collect_controls(ui::Node& node, std::vector<ui::Node*>& out) {
    if (is_control(node)) {
        out.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_controls(*child, out);
    }
}

void cover(ui::Node& node, rhi::Rect bounds) {
    node.set_bounds(bounds);
    for (const auto& child : node.children()) {
        cover(*child, rhi::Rect{0, 0, bounds.width, bounds.height});
    }
}

// Shrinks every container to the union of its children (they all sit at the origin of their
// parent), so no sibling container covers another's controls: the hit test returns the topmost
// node under the point, and a later full-window row would otherwise win over an earlier one.
// Nodes that hold no control collapse to nothing.
bool fit(ui::Node& node) {
    if (is_control(node)) {
        return true;
    }
    // A Component host wrapping a control keeps the bounds the fixture gave it.
    for (const auto& child : node.children()) {
        if (is_control(*child)) {
            return true;
        }
    }
    std::vector<ui::Node*> kept;
    for (const auto& child : node.children()) {
        if (fit(*child)) {
            kept.push_back(child.get());
        }
    }
    if (kept.empty()) {
        node.set_bounds(rhi::Rect{0, 0, 0, 0});
        return false;
    }
    float left = 1e9f;
    float top = 1e9f;
    float right = 0;
    float bottom = 0;
    for (const ui::Node* child : kept) {
        const rhi::Rect& box = child->bounds();
        left = std::min(left, box.x);
        top = std::min(top, box.y);
        right = std::max(right, box.x + box.width);
        bottom = std::max(bottom, box.y + box.height);
    }
    // Children are positioned in this node's frame, so moving the node's origin to the union's
    // corner rebases them by the same amount.
    for (ui::Node* child : kept) {
        const rhi::Rect box = child->bounds();
        child->set_bounds(rhi::Rect{box.x - left, box.y - top, box.width, box.height});
    }
    node.set_bounds(rhi::Rect{left, top, right - left, bottom - top});
    return true;
}

std::string aria(const ui::Node& node, std::string_view name) {
    for (const ui::AriaAttribute& attribute : node.aria_attributes()) {
        if (attribute.name == name) {
            return std::string(attribute.value);
        }
    }
    return "<absent>";
}

ui::InputEvent keyboard(ui::InputEventType type, ui::KeyCode code) {
    ui::InputEvent event;
    event.type = type;
    event.key = code;
    return event;
}

ui::InputEvent pointer(ui::InputEventType type, float x, float y) {
    ui::InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    event.pointer_id = 1;
    event.pointer_kind = ui::PointerKind::Mouse;
    event.button = ui::PointerButton::Left;
    event.is_primary = true;
    return event;
}

// Controls in page order: switches 0-3 (Wi-Fi, Sound, locked off, locked on), checkboxes 4-7
// (terms, newsletter, mixed, locked), radios 8-11 (Free, Team, Enterprise, disabled Legacy).
enum Index {
    kWifi,
    kSound,
    kLockedOff,
    kLockedOn,
    kTerms,
    kNews,
    kMixed,
    kLockedBox,
    kFree,
    kTeam,
    kEnterprise,
    kLegacy
};

struct Fixture {
    Fixture() {
        page.build(root);
        page.attach_scheduler(scheduler);
        page.apply_props();
        scheduler.flush();
        collect_controls(root, controls);
        cover(root, rhi::Rect{0, 0, 400, 700});
        float y = 10.0f;
        for (ui::Node* control : controls) {
            // The Component host wraps each control's root; both cover exactly the control.
            if (ui::Node* host = control->parent()) {
                host->set_bounds({10, y, 160, 32});
            }
            cover(*control, rhi::Rect{0, 0, 160, 32});
            y += 40.0f;
        }
        fit(root);
        router.focus_manager().set_root(root);
    }

    // What the host does each frame: the setters marked tickets, the flush runs the updates.
    void refresh() {
        scheduler.flush();
    }

    void press(ui::KeyCode code) {
        router.dispatch(root, keyboard(ui::InputEventType::KeyDown, code));
        router.dispatch(root, keyboard(ui::InputEventType::KeyUp, code));
        refresh();
    }

    void click(std::size_t index) {
        const float y = 10.0f + 40.0f * static_cast<float>(index) + 16.0f;
        router.dispatch(root, pointer(ui::InputEventType::PointerDown, 20, y));
        router.dispatch(root, pointer(ui::InputEventType::PointerUp, 20, y));
        refresh();
    }

    ui::Node& at(std::size_t index) {
        return *controls[index];
    }

    ui::Node root{1000};
    ui::UpdateScheduler scheduler;
    gallery::ControlsPage page;
    std::vector<ui::Node*> controls;
    ui::InputRouter router;
};

} // namespace

int main() {
    Fixture f;
    check(f.controls.size() == 12, "twelve controls: " + std::to_string(f.controls.size()));
    if (f.controls.size() != 12) {
        return 1;
    }
    for (std::size_t i = 0; i < f.controls.size(); ++i) {
        const std::string_view want = i < 4 ? "switch" : i < 8 ? "checkbox" : "radio";
        check(f.at(i).role() == want,
              "control " + std::to_string(i) + " is a " + std::string(want));
        check(!aria(f.at(i), "aria-label").empty() && aria(f.at(i), "aria-label") != "<absent>",
              "control " + std::to_string(i) + " has an accessible name");
    }

    // Initial state mirrors the caller's state.
    check(aria(f.at(kWifi), "aria-checked") == "false", "Wi-Fi starts off");
    check(aria(f.at(kSound), "aria-checked") == "true", "Sound starts on");
    check(aria(f.at(kLockedOn), "aria-checked") == "true", "the locked switch is on");
    check(aria(f.at(kMixed), "aria-checked") == "mixed", "an indeterminate Checkbox is mixed");
    check(aria(f.at(kTeam), "aria-checked") == "true", "the Team radio starts selected");
    check(aria(f.at(kFree), "aria-checked") == "false", "the Free radio starts unselected");
    check(aria(f.at(kLockedOff), "aria-disabled") == "true", "a disabled Switch reports it");
    check(aria(f.at(kWifi), "aria-disabled") == "false", "an enabled Switch reports it");

    // Switch: Space and Enter toggle the focused Switch; the state reaches the node.
    f.router.focus_manager().request_focus(&f.at(kWifi));
    f.press(key::kSpace);
    check(f.page.wifi(), "Space turns Wi-Fi on");
    check(aria(f.at(kWifi), "aria-checked") == "true", "aria-checked follows: on");
    f.press(key::kEnter);
    check(!f.page.wifi(), "Enter turns Wi-Fi off");
    check(aria(f.at(kWifi), "aria-checked") == "false", "aria-checked follows: off");
    f.click(kSound);
    check(!f.page.sound(), "a pointer click toggles Sound off");
    check(aria(f.at(kSound), "aria-checked") == "false", "the click reaches aria-checked");

    // A disabled Switch ignores keys and pointer.
    f.router.focus_manager().request_focus(&f.at(kLockedOff));
    f.press(key::kSpace);
    f.press(key::kEnter);
    f.click(kLockedOff);
    check(!f.page.wifi(), "a disabled Switch never calls onToggle");
    check(aria(f.at(kLockedOff), "aria-checked") == "false", "a disabled Switch keeps its state");

    // Checkbox: Space and a click toggle, Enter does not.
    f.router.focus_manager().request_focus(&f.at(kTerms));
    f.press(key::kEnter);
    check(!f.page.terms(), "Enter does not toggle a Checkbox");
    f.press(key::kSpace);
    check(f.page.terms() && aria(f.at(kTerms), "aria-checked") == "true", "Space checks it");
    f.click(kNews);
    check(!f.page.news() && aria(f.at(kNews), "aria-checked") == "false", "a click unchecks it");
    f.click(kLockedBox);
    check(!f.page.news(), "a disabled Checkbox never calls onToggle");

    // Radio: a click selects, the previous one deselects, a disabled one never selects.
    f.click(kFree);
    check(f.page.plan() == 0, "clicking Free selects plan 0");
    check(aria(f.at(kFree), "aria-checked") == "true" &&
              aria(f.at(kTeam), "aria-checked") == "false",
          "selection moves between Radios");
    f.router.focus_manager().request_focus(&f.at(kEnterprise));
    f.press(key::kSpace);
    check(f.page.plan() == 2, "Space selects the focused Radio");
    f.router.focus_manager().request_focus(&f.at(kLegacy));
    f.press(key::kSpace);
    f.click(kLegacy);
    check(f.page.plan() == 2, "a disabled Radio never calls onSelect");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "controls_runtime: ok\n";
    return 0;
}
