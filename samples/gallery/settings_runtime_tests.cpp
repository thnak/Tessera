// SPDX-License-Identifier: MIT

// Headless interaction test of the Settings starter template (issues #240 and #527), through the
// generated `SettingsPage` and the real `InputRouter`: pointer and keyboard reach the page state
// through library Switch, Checkbox, Radio, Details and Button, Save commits the edits, and the
// accessibility contract (role, name, `aria-checked`, `aria-expanded`, `aria-valuenow`) follows the
// state. Building this file is also the codegen compile test of the template. No GPU, no clock.

#include "SettingsPage.hpp"
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
    return node.focusable() &&
           (role == "switch" || role == "checkbox" || role == "radio" || role == "button");
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

// Controls in page order: switches 0-1 (email, push), checkbox 2 (digest), radios 3-4 (Comfortable,
// Compact), switch 5 (reduce motion), summary 6 (Advanced), checkbox 7 (diagnostics), button 8
// (Save).
enum Index {
    kEmail,
    kPush,
    kDigest,
    kComfortable,
    kCompact,
    kMotion,
    kAdvanced,
    kTelemetry,
    kSave
};

void collect_bars(ui::Node& node, std::vector<ui::Node*>& out) {
    if (node.role() == "progressbar") {
        out.push_back(&node);
    }
    for (const auto& child : node.children()) {
        collect_bars(*child, out);
    }
}

struct Fixture {
    Fixture() {
        page.build(root);
        page.attach_scheduler(scheduler);
        page.apply_props();
        scheduler.flush();
        collect_controls(root, controls);
        collect_bars(root, bars);
        cover(root, rhi::Rect{0, 0, 400, 900});
        float y = 10.0f;
        for (ui::Node* control : controls) {
            if (ui::Node* host = control->parent()) {
                host->set_bounds({10, y, 160, 32});
            }
            cover(*control, rhi::Rect{0, 0, 160, 32});
            y += 40.0f;
        }
        fit(root);
        router.focus_manager().set_root(root);
    }

    void press(ui::KeyCode code) {
        router.dispatch(root, keyboard(ui::InputEventType::KeyDown, code));
        router.dispatch(root, keyboard(ui::InputEventType::KeyUp, code));
        scheduler.flush();
    }

    void click(std::size_t index) {
        const float y = 10.0f + 40.0f * static_cast<float>(index) + 16.0f;
        router.dispatch(root, pointer(ui::InputEventType::PointerDown, 20, y));
        router.dispatch(root, pointer(ui::InputEventType::PointerUp, 20, y));
        scheduler.flush();
    }

    ui::Node& at(std::size_t index) {
        return *controls[index];
    }

    ui::Node root{1000};
    ui::UpdateScheduler scheduler;
    gallery::SettingsPage page;
    std::vector<ui::Node*> controls;
    std::vector<ui::Node*> bars;
    ui::InputRouter router;
};

} // namespace

int main() {
    Fixture f;
    check(f.controls.size() == 9, "nine controls: " + std::to_string(f.controls.size()));
    check(f.bars.size() == 1, "one progress bar: " + std::to_string(f.bars.size()));
    if (f.controls.size() != 9 || f.bars.size() != 1) {
        return 1;
    }
    const std::string_view roles[] = {"switch", "switch", "checkbox", "radio", "radio",
                                      "switch", "button", "checkbox", "button"};
    for (std::size_t i = 0; i < f.controls.size(); ++i) {
        check(f.at(i).role() == roles[i], "control " + std::to_string(i) + " has the library role");
        check(aria(f.at(i), "aria-label") != "<absent>" && !aria(f.at(i), "aria-label").empty(),
              "control " + std::to_string(i) + " has an accessible name");
    }
    check(aria(f.at(kEmail), "aria-label") == "Email notifications", "the label is the name");
    check(aria(*f.bars[0], "aria-valuenow") == "62" && aria(*f.bars[0], "aria-valuemax") == "100",
          "storage exposes 62 of 100");

    // Initial state, as the page declares it.
    check(aria(f.at(kEmail), "aria-checked") == "true", "email starts on");
    check(aria(f.at(kPush), "aria-checked") == "false", "push starts off");
    check(aria(f.at(kComfortable), "aria-checked") == "true", "Comfortable starts selected");
    check(f.page.edits() == 0 && f.page.saves() == 0, "no edits and no saves yet");

    // Pointer and keyboard toggle, each edit is counted, Save commits.
    f.click(kPush);
    check(aria(f.at(kPush), "aria-checked") == "true" && f.page.edits() == 1,
          "a click turns push on");
    f.router.focus_manager().request_focus(&f.at(kEmail));
    f.press(key::kSpace);
    check(aria(f.at(kEmail), "aria-checked") == "false" && f.page.edits() == 2,
          "Space turns email off");
    f.router.focus_manager().request_focus(&f.at(kMotion));
    f.press(key::kEnter);
    check(aria(f.at(kMotion), "aria-checked") == "true" && f.page.edits() == 3,
          "Enter turns motion on");
    f.click(kDigest);
    check(aria(f.at(kDigest), "aria-checked") == "false", "a click clears the digest checkbox");
    f.click(kCompact);
    check(aria(f.at(kCompact), "aria-checked") == "true" &&
              aria(f.at(kComfortable), "aria-checked") == "false",
          "choosing Compact moves the selection");
    check(f.page.edits() == 5 && f.page.saves() == 0, "five edits, still unsaved");
    f.click(kSave);
    check(f.page.saves() == 5, "Save commits the edit count");
    f.router.focus_manager().request_focus(&f.at(kSave));
    f.press(key::kEnter);
    check(f.page.saves() == 5, "a second Save with no new edit changes nothing");

    // The Advanced section opens and its checkbox works.
    check(aria(f.at(kAdvanced), "aria-expanded") == "false", "Advanced starts closed");
    f.router.focus_manager().request_focus(&f.at(kAdvanced));
    f.press(key::kEnter);
    check(aria(f.at(kAdvanced), "aria-expanded") == "true", "Enter opens Advanced");
    f.router.focus_manager().request_focus(&f.at(kTelemetry));
    f.press(key::kSpace);
    check(aria(f.at(kTelemetry), "aria-checked") == "true" && f.page.edits() == 6,
          "the diagnostics checkbox is an edit");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "settings_runtime: ok\n";
    return 0;
}
