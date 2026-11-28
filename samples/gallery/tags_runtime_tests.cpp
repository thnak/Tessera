// SPDX-License-Identifier: MIT

// Headless interaction test of the controls the HTML form tags lower onto (issue #1041, ADR-059
// group E), through the generated `TagsPage` and the real `InputRouter`: `input type="checkbox"`,
// `"radio"` and `"range"` and `details`/`summary` take keyboard and pointer, the caller's
// `onChange` reaches its state, a disabled control never calls it, and the semantics (role,
// `aria-checked`, `aria-valuenow`, `aria-expanded`) follow the state. No GPU, no clock.

#include "TagsPage.hpp"
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
           (role == "checkbox" || role == "radio" || role == "slider" || role == "button");
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

// Controls in page order: checkboxes 0-2 (terms, newsletter, locked), radios 3-5 (Free, Team,
// Enterprise), ranges 6-7 (Volume, Brightness), summaries 8-9 (Shipping open, Returns closed).
enum Index {
    kTerms,
    kNews,
    kLocked,
    kFree,
    kTeam,
    kEnterprise,
    kVolume,
    kBrightness,
    kShipping,
    kReturns
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
    gallery::TagsPage page;
    std::vector<ui::Node*> controls;
    std::vector<ui::Node*> bars;
    ui::InputRouter router;
};

} // namespace

int main() {
    Fixture f;
    check(f.controls.size() == 10, "ten controls: " + std::to_string(f.controls.size()));
    check(f.bars.size() == 6, "six progress and meter bars: " + std::to_string(f.bars.size()));
    if (f.controls.size() != 10 || f.bars.size() != 6) {
        return 1;
    }
    for (std::size_t i = 0; i < f.controls.size(); ++i) {
        const std::string_view want = i < 3   ? "checkbox"
                                      : i < 6 ? "radio"
                                      : i < 8 ? "slider"
                                              : "button";
        check(f.at(i).role() == want,
              "control " + std::to_string(i) + " is a " + std::string(want));
        check(aria(f.at(i), "aria-label") != "<absent>" && !aria(f.at(i), "aria-label").empty(),
              "control " + std::to_string(i) + " has an accessible name");
    }
    check(aria(f.at(kTerms), "aria-label") == "Accept terms", "aria-label became the label prop");

    // Checkbox: onChange is the page's toggle; a click and Space flip it, a disabled one never
    // does.
    check(aria(f.at(kTerms), "aria-checked") == "false", "terms start unchecked");
    check(aria(f.at(kNews), "aria-checked") == "true", "newsletter starts checked");
    check(aria(f.at(kLocked), "aria-disabled") == "true", "the locked checkbox is disabled");
    f.click(kTerms);
    check(f.page.terms() && aria(f.at(kTerms), "aria-checked") == "true", "a click checks terms");
    f.router.focus_manager().request_focus(&f.at(kNews));
    f.press(key::kSpace);
    check(!f.page.news() && aria(f.at(kNews), "aria-checked") == "false", "Space unchecks news");
    f.click(kLocked);
    f.router.focus_manager().request_focus(&f.at(kLocked));
    f.press(key::kSpace);
    check(aria(f.at(kLocked), "aria-checked") == "true", "a disabled checkbox keeps its state");

    // Radio: checked became selected, onChange became onSelect.
    check(aria(f.at(kTeam), "aria-checked") == "true" &&
              aria(f.at(kFree), "aria-checked") == "false",
          "the Team radio starts selected");
    f.click(kFree);
    check(f.page.plan() == 0 && aria(f.at(kFree), "aria-checked") == "true" &&
              aria(f.at(kTeam), "aria-checked") == "false",
          "a click moves the selection");
    f.router.focus_manager().request_focus(&f.at(kEnterprise));
    f.press(key::kSpace);
    check(f.page.plan() == 2, "Space selects the focused radio");

    // Range: min/max/step/defaultValue reached the slider; the keys move it within the bounds.
    check(aria(f.at(kVolume), "aria-valuenow") == "40" &&
              aria(f.at(kVolume), "aria-valuemax") == "100",
          "Volume starts at 40 of 100: " + aria(f.at(kVolume), "aria-valuenow") + " of " +
              aria(f.at(kVolume), "aria-valuemax"));
    f.router.focus_manager().request_focus(&f.at(kVolume));
    f.press(key::kArrowRight);
    check(aria(f.at(kVolume), "aria-valuenow") == "45", "ArrowRight adds the page's step of 5");
    f.press(key::kEnd);
    check(aria(f.at(kVolume), "aria-valuenow") == "100", "End jumps to max");
    f.press(key::kHome);
    check(aria(f.at(kVolume), "aria-valuenow") == "0", "Home jumps to min");
    check(aria(f.at(kBrightness), "aria-valuenow") == "10" &&
              aria(f.at(kBrightness), "aria-valuemax") == "10",
          "Brightness starts at its max");

    // Progress and meter: the bounds and value are exposed, and they are not focusable.
    check(aria(*f.bars[0], "aria-valuenow") == "25" && aria(*f.bars[0], "aria-valuemax") == "100",
          "progress exposes its value and max");
    check(aria(*f.bars[3], "aria-valuenow") == "50", "meter exposes its value");
    check(!f.bars[0]->focusable(), "a progress bar is not focusable");

    // details: open became defaultOpen; the summary is the button, Space and Enter flip it.
    check(aria(f.at(kShipping), "aria-expanded") == "true", "the open details starts open");
    check(aria(f.at(kReturns), "aria-expanded") == "false", "the closed details starts closed");
    check(aria(f.at(kShipping), "aria-label") == "Shipping", "the summary text is the name");
    f.router.focus_manager().request_focus(&f.at(kShipping));
    f.press(key::kSpace);
    check(aria(f.at(kShipping), "aria-expanded") == "false", "Space closes it");
    f.router.focus_manager().request_focus(&f.at(kReturns));
    f.press(key::kEnter);
    check(aria(f.at(kReturns), "aria-expanded") == "true", "Enter opens the focused summary");

    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "tags_runtime: ok\n";
    return 0;
}
