// SPDX-License-Identifier: MIT
//
// Overlay placement, dismissal and the input hook through a real AppHost (issue #302 slice 2,
// docs/specs/overlays-and-layers-v1.md sections 3, 4 and 8.1): anchored positioning that follows
// the safe area, Escape and back ordering, press-outside rules, window blur, and the pump filter.

#include "overlay_test_fixture.hpp"
#include "tessera/ui/input_pump.hpp"

#include <vector>

namespace {

using namespace overlay_test;
using tessera::ui::OverlayAlign;
using tessera::ui::OverlayAnchor;
using tessera::ui::OverlayDismissReason;
using tessera::ui::OverlayPlacement;
using tessera::ui::OverlaySide;

constexpr NodeId kAnchor = 9;

bool near(float a, float b) {
    return std::fabs(a - b) < 0.5f;
}

InputEvent key_event(InputEventType type, tessera::ui::KeyCode code) {
    InputEvent event;
    event.type = type;
    event.key = code;
    return event;
}

InputEvent press_at(float x, float y) {
    InputEvent event;
    event.type = InputEventType::PointerDown;
    event.x = x;
    event.y = y;
    return event;
}

// A 100x100 anchor at the top-left of the window, under the base content.
Node& add_anchor(Fixture& f) {
    Node* base_button = f.root.children()[0]->children()[0].get();
    Node& anchor = base_button->add_child(std::make_unique<Node>(kAnchor));
    anchor.set_element_kind("View");
    anchor.set_class_name("box");
    return anchor;
}

OverlaySpec placed(const char* id, OverlayLayer layer, NodeId box_id, OverlayPlacement where) {
    OverlaySpec value = spec(id, layer, box_id);
    value.placement = where;
    return value;
}

OverlayPlacement against(OverlayAnchor anchor, OverlaySide side) {
    OverlayPlacement where;
    where.anchor = anchor;
    where.side = side;
    return where;
}

void test_anchored_placement() {
    Fixture f;
    Node& anchor = add_anchor(f);
    f.frame();
    const auto handle =
        f.host->open_overlay(placed("menu", OverlayLayer::Overlay, 20,
                                    against(OverlayAnchor::of(kAnchor), OverlaySide::Bottom)));
    f.frame();
    Node* content = f.host->overlays().content(handle);
    expect(content != nullptr, "the anchored overlay mounts");
    if (content == nullptr) {
        return;
    }
    expect(near(content->world_bounds().x, 0.0f) && near(content->world_bounds().y, 100.0f),
           "it sits below its anchor on the frame it opens");
    f.frame();
    const auto settled = f.frame();
    expect(!settled.frame_due && !settled.tick.needs_layout,
           "a placed overlay that stopped moving is idle");

    // The anchor moves (its box grows), the overlay follows at the frame boundary.
    anchor.set_inline_style("height", "160px");
    f.frame();
    f.frame();
    expect(near(content->world_bounds().y, 160.0f), "it follows the anchor's layout");

    // The anchor leaves the scene: the default is to close.
    static_cast<void>(anchor.parent()->remove_child(anchor));
    f.frame();
    f.frame();
    expect(!f.host->overlays().is_open(handle) && f.host->overlays().size() == 0,
           "removing the anchor closes the overlay");
}

void test_flip_and_safe_area() {
    Fixture f;
    f.frame();
    // A trigger near the bottom of the window: the popover goes above it.
    OverlayPlacement where =
        against(OverlayAnchor::at({150.0f, 760.0f, 100.0f, 30.0f}), OverlaySide::Bottom);
    const auto popover = f.host->open_overlay(placed("pop", OverlayLayer::Overlay, 20, where));
    f.frame();
    Node* content = f.host->overlays().content(popover);
    expect(content != nullptr && near(content->world_bounds().y, 660.0f),
           "no room below the anchor: it flips above");

    // A status bar and a keyboard: a top-centre toast stays clear of both, and follows them.
    tessera::hal::WindowMetrics metrics;
    metrics.logical_width = 400;
    metrics.logical_height = 800;
    metrics.insets = {0, 44, 0, 0};
    f.host->set_window_metrics(metrics);
    OverlayPlacement top = against(OverlayAnchor::window(), OverlaySide::Top);
    top.gap = 8.0f;
    const auto toast = f.host->open_overlay(placed("toast", OverlayLayer::Toast, 40, top));
    f.frame();
    Node* toast_content = f.host->overlays().content(toast);
    expect(toast_content != nullptr && near(toast_content->world_bounds().x, 150.0f) &&
               near(toast_content->world_bounds().y, 52.0f),
           "top-centre, below the status bar");
    metrics.insets = {0, 80, 0, 0};
    f.host->set_window_metrics(metrics);
    for (int i = 0; i < 120; ++i) {
        f.frame();
    }
    expect(toast_content != nullptr && near(toast_content->world_bounds().y, 88.0f),
           "it follows the inset as it eases");
    const auto idle = f.frame();
    expect(!idle.frame_due, "and settles to idle");

    OverlaySpec live = spec("status", OverlayLayer::Toast, 41);
    const auto status = f.host->open_overlay(std::move(live));
    f.frame();
    const Node* status_content = f.host->overlays().content(status);
    bool polite = false;
    if (status_content != nullptr) {
        for (const auto& attribute : status_content->aria_attributes()) {
            polite = polite || (attribute.name == "aria-live" && attribute.value == "polite");
        }
    }
    expect(polite, "a toast is a polite live region");
}

void test_escape_and_back() {
    Fixture f;
    std::vector<OverlayDismissReason> reasons;
    const auto record = [&reasons](OverlayHandle, OverlayDismissReason reason) {
        reasons.push_back(reason);
    };
    OverlaySpec menu = spec("menu", OverlayLayer::Overlay, 10);
    menu.on_dismiss = record;
    OverlaySpec hint = spec("hint", OverlayLayer::Tooltip, 30);
    hint.on_dismiss = record;
    f.host->open_overlay(std::move(menu));
    f.host->open_overlay(std::move(hint));
    f.host->open_overlay(spec("toast", OverlayLayer::Toast, 40));
    f.frame();

    expect(f.host->handle_overlay_input(key_event(InputEventType::KeyDown, key::kEscape)),
           "Escape is consumed when an overlay takes it");
    expect(reasons.size() == 1 && reasons[0] == OverlayDismissReason::Escape,
           "the tooltip, the topmost dismissible entry, went first");
    expect(f.host->handle_overlay_input(key_event(InputEventType::KeyUp, key::kEscape)),
           "its release is swallowed too");
    expect(!f.host->handle_overlay_input(key_event(InputEventType::KeyUp, key::kEscape)), "once");
    f.frame();
    expect(f.hit(10, 10) != nullptr && f.hit(10, 10)->id() == 40, "the toast is not dismissible");

    expect(f.host->overlays().handle_back(), "back dismisses the next one down");
    expect(reasons.size() == 2 && reasons[1] == OverlayDismissReason::Back, "with its own reason");
    f.frame();
    expect(!f.host->handle_overlay_input(key_event(InputEventType::KeyDown, key::kEscape)),
           "nothing dismissible is left, so Escape passes through");
    expect(!f.host->handle_overlay_input(key_event(InputEventType::KeyUp, key::kEscape)),
           "and so does its release");
    expect(f.host->overlays().size() == 1, "only the toast remains");

    // A dialog that did not opt in owns the keyboard: the menu under it must not react.
    Fixture g;
    g.host->open_overlay(spec("menu", OverlayLayer::Overlay, 10));
    OverlaySpec dialog = spec("dialog", OverlayLayer::Modal, 50);
    dialog.dismiss_on_escape = false;
    g.host->open_overlay(std::move(dialog));
    g.frame();
    expect(!g.host->handle_overlay_input(key_event(InputEventType::KeyDown, key::kEscape)),
           "a modal that kept Escape stops the search");
    expect(g.host->overlays().size() == 2, "nothing closed");
}

void test_press_outside() {
    Fixture f;
    f.host->open_overlay(spec("menu", OverlayLayer::Overlay, 10));
    f.frame();
    expect(!f.host->handle_overlay_input(press_at(10, 10)) && f.host->overlays().size() == 1,
           "a press inside the content leaves it open");
    expect(!f.host->handle_overlay_input(press_at(300, 300)),
           "an outside press is never consumed: it still reaches the scene");
    expect(f.host->overlays().size() == 0, "but it closed the menu");
    f.frame();

    // The trigger toggles its own popover, so a press on the anchor is not an outside press.
    Fixture g;
    add_anchor(g);
    g.frame();
    g.host->open_overlay(placed("pop", OverlayLayer::Overlay, 20,
                                against(OverlayAnchor::of(kAnchor), OverlaySide::Bottom)));
    g.frame();
    g.host->handle_overlay_input(press_at(10, 10));
    expect(g.host->overlays().size() == 1, "a press on the anchor keeps it open");
    g.host->handle_overlay_input(press_at(300, 700));
    expect(g.host->overlays().size() == 0, "a press elsewhere closes it");

    // A dialog is inert beneath and, by default, does not close on an outside press.
    Fixture h;
    std::vector<OverlayDismissReason> reasons;
    h.host->open_overlay(spec("menu", OverlayLayer::Overlay, 10));
    OverlaySpec dialog = spec("dialog", OverlayLayer::Modal, 50);
    dialog.on_dismiss = [&reasons](OverlayHandle, OverlayDismissReason reason) {
        reasons.push_back(reason);
    };
    const auto handle = h.host->open_overlay(std::move(dialog));
    h.frame();
    h.host->handle_overlay_input(press_at(300, 700)); // the scrim
    expect(h.host->overlays().size() == 2,
           "the scrim press closes neither the dialog nor the menu");
    h.host->overlays().close(handle);
    h.frame();

    Fixture i;
    i.host->open_overlay(spec("menu", OverlayLayer::Overlay, 10));
    OverlaySpec sheet = spec("sheet", OverlayLayer::Modal, 50);
    sheet.dismiss_on_outside = true;
    sheet.on_dismiss = [&reasons](OverlayHandle, OverlayDismissReason reason) {
        reasons.push_back(reason);
    };
    i.host->open_overlay(std::move(sheet));
    i.frame();
    i.host->handle_overlay_input(press_at(300, 700));
    expect(i.host->overlays().size() == 1 && reasons.size() == 1 &&
               reasons[0] == OverlayDismissReason::Outside,
           "a dialog that opted in closes on the scrim, and the menu beneath stays");

    // A press on a toast never closes what sits below it.
    Fixture j;
    j.host->open_overlay(spec("menu", OverlayLayer::Overlay, 10));
    j.host->open_overlay(placed("toast", OverlayLayer::Toast, 40,
                                against(OverlayAnchor::window(), OverlaySide::Bottom)));
    j.frame();
    j.host->handle_overlay_input(press_at(200, 760));
    expect(j.host->overlays().size() == 2, "the toast is excluded from outside dismissal");
    j.host->handle_overlay_input(press_at(300, 300));
    expect(j.host->overlays().size() == 1, "while the base still dismisses the menu");
}

void test_blur_closes_transient_layers() {
    Fixture f;
    f.host->open_overlay(spec("menu", OverlayLayer::Overlay, 10));
    OverlaySpec pinned = spec("pinned", OverlayLayer::Overlay, 11);
    pinned.persistent = true;
    f.host->open_overlay(std::move(pinned));
    f.host->open_overlay(spec("hint", OverlayLayer::Tooltip, 30));
    f.host->open_overlay(spec("dialog", OverlayLayer::Modal, 50));
    f.host->open_overlay(spec("toast", OverlayLayer::Toast, 40));
    f.frame();
    InputEvent blur;
    blur.type = InputEventType::Blur;
    expect(!f.host->handle_overlay_input(blur), "blur is never consumed");
    expect(f.host->overlays().size() == 3, "the menu and the tooltip closed");
    f.frame();
    expect(f.host->overlays().layer_node(OverlayLayer::Modal)->children().size() == 1,
           "a dialog survives a blur");
}

bool g_filter_saw_escape = false;

bool escape_filter(void*, Node&, const InputEvent& event) {
    g_filter_saw_escape = event.type == InputEventType::KeyDown && event.key == key::kEscape;
    return g_filter_saw_escape;
}

void test_pump_filter() {
    Node root(1);
    root.set_bounds({0, 0, 100, 100});
    tessera::runtime::ManualClock clock;
    tessera::runtime::RuntimeLoop loop(clock);
    tessera::ui::InputPump pump;
    pump.set_input_filter(&escape_filter);
    pump.queue().push(key_event(InputEventType::KeyDown, key::kEscape));
    pump.queue().push(key_event(InputEventType::KeyDown, key::kTab));
    const auto stats = pump.pump(root, loop);
    expect(stats.consumed == 2, "both events were read");
    expect(stats.dispatched == 1 && stats.dropped == 1,
           "the consumed one counts as handled and never reaches the router; the other is "
           "dispatched normally (and finds no target)");
    pump.set_input_filter(nullptr);
    g_filter_saw_escape = false;
    pump.queue().push(key_event(InputEventType::KeyDown, key::kEscape));
    pump.pump(root, loop);
    expect(!g_filter_saw_escape, "a cleared filter is not called");
}

} // namespace

int main() {
    suite = "overlay_dismiss";
    test_anchored_placement();
    test_flip_and_safe_area();
    test_escape_and_back();
    test_press_outside();
    test_blur_closes_transient_layers();
    test_pump_filter();
    if (failures != 0) {
        std::cerr << "overlay_dismiss: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
