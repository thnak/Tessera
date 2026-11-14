// SPDX-License-Identifier: MIT
//
// Overlay layers (issue #302, docs/specs/overlays-and-layers-v1.md): layer order, (priority,
// open order) inside a layer, handle lifetime, frame-boundary opens and closes, hit-test order, the
// modal scrim, focus trap and restore, the semantics tree, mount failures and an idle host. The
// scene is a real AppHost, so layout and style come from the shipped pipeline.

#include "a11y_test_helpers.hpp"
#include "tessera/app/host.hpp"
#include "tessera/runtime/clock.hpp"
#include "tessera/ui/key_code.hpp"
#include "tessera/ui/overlay/overlay.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/input_dispatch.hpp"

#include <algorithm>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

using tessera::ui::FocusManager;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::OverlayHandle;
using tessera::ui::OverlayHost;
using tessera::ui::OverlayInfo;
using tessera::ui::OverlayLayer;
using tessera::ui::OverlaySpec;
namespace key = tessera::ui::key;

int failures = 0;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "overlay_host: " << message << "\n";
        ++failures;
    }
    return condition;
}

constexpr double kFrame = 1.0 / 60.0;
constexpr NodeId kBase = 2;
constexpr NodeId kBaseButton = 3;

// A 100x100 box at the top-left of whichever overlay content root it is mounted in.
void box(Node& content, NodeId id, bool focusable = false) {
    Node& node = content.add_child(std::make_unique<Node>(id));
    node.set_element_kind("View");
    node.set_class_name("box");
    node.set_focusable(focusable);
    node.set_tab_index(0);
}

OverlaySpec spec(const char* id, OverlayLayer layer, NodeId box_id, int priority = 0,
                 bool focusable = false) {
    OverlaySpec value;
    value.id = id;
    value.layer = layer;
    value.priority = priority;
    value.mount = [box_id, focusable](Node& content) { box(content, box_id, focusable); };
    return value;
}

// root -> base (fills the window, holds one focusable button).
struct Fixture {
    Fixture()
        : root(1),
          loop(clock) {
        Node& base = root.add_child(std::make_unique<Node>(kBase));
        base.set_element_kind("View");
        base.set_class_name("fill");
        Node& button = base.add_child(std::make_unique<Node>(kBaseButton));
        button.set_element_kind("View");
        button.set_class_name("fill");
        button.set_focusable(true);
        button.set_tab_index(0);
        tessera::app::HostConfig config;
        config.width = 400.0f;
        config.height = 800.0f;
        host = std::make_unique<tessera::app::AppHost>(
            root,
            tessera::ui::style::compile_stylesheet(
                ".fill { flex-grow: 1; display: flex; flex-direction: column; }"
                ".box { width: 100px; height: 100px; }"),
            loop, config);
    }

    tessera::app::HostFrame frame() {
        const auto result = host->begin_frame(kFrame);
        host->end_frame();
        return result;
    }

    // The deepest node under the point, as the pointer router would pick it.
    Node* hit(float x, float y) {
        return root.hit_test_deep(x, y);
    }

    tessera::runtime::ManualClock clock;
    Node root;
    tessera::runtime::RuntimeLoop loop;
    std::unique_ptr<tessera::app::AppHost> host;
};

bool has_id(const tessera::ui::a11y::A11ySnapshot& snapshot, NodeId id) {
    return std::ranges::any_of(snapshot.nodes, [id](const auto& node) { return node.id == id; });
}

void test_lazy_and_frame_boundary() {
    Fixture f;
    f.frame();
    expect(f.root.children().size() == 1, "a scene that opens nothing gains no nodes");
    expect(f.host->overlays().layer_node(OverlayLayer::Modal) == nullptr,
           "no layer node before the first entry");

    int mounts = 0;
    OverlaySpec s = spec("menu", OverlayLayer::Overlay, 100);
    s.mount = [&mounts](Node& content) {
        ++mounts;
        box(content, 100);
    };
    const OverlayHandle handle = f.host->open_overlay(std::move(s));
    expect(static_cast<bool>(handle), "the handle is valid at once");
    expect(mounts == 0 && !f.host->overlays().is_open(handle), "nothing mounts mid-frame");
    expect(f.root.children().size() == 1, "the tree is untouched until the frame boundary");
    expect(f.host->overlays().size() == 1, "a pending entry counts");

    const auto frame = f.frame();
    expect(mounts == 1 && f.host->overlays().is_open(handle), "the next frame mounts it once");
    expect(frame.tick.needs_layout, "mounting asks for layout");
    expect(f.host->overlays().content(handle) != nullptr, "the content root is reachable");
    Node* layer = f.host->overlays().layer_node(OverlayLayer::Overlay);
    expect(layer != nullptr && f.root.children().size() == 2 && f.root.children()[1].get() == layer,
           "the layer is a child of the root after the base content");
    expect(layer != nullptr && layer->world_bounds().width == 400.0f &&
               layer->world_bounds().height == 800.0f,
           "a layer fills the window");
    f.frame();
    expect(mounts == 1, "a settled entry is not mounted again");
}

void test_layer_order_and_hit_testing() {
    Fixture f;
    // Opened out of order on purpose: stacking comes from the layer, not from the open order.
    const auto toast = f.host->open_overlay(spec("toast", OverlayLayer::Toast, 40));
    const auto overlay = f.host->open_overlay(spec("overlay", OverlayLayer::Overlay, 10));
    const auto tooltip = f.host->open_overlay(spec("tooltip", OverlayLayer::Tooltip, 30));
    f.frame();
    Node* hit = f.hit(10, 10);
    expect(hit != nullptr && hit->id() == 40, "the toast layer is hit first");
    f.host->close_overlay(toast);
    f.frame();
    hit = f.hit(10, 10);
    expect(hit != nullptr && hit->id() == 30, "closing the toast reveals the tooltip");
    f.host->close_overlay(tooltip);
    f.frame();
    hit = f.hit(10, 10);
    expect(hit != nullptr && hit->id() == 10, "then the overlay layer");
    f.host->close_overlay(overlay);
    f.frame();
    hit = f.hit(10, 10);
    expect(hit != nullptr && hit->id() == kBaseButton, "then the base content");
    hit = f.hit(300, 300);
    expect(hit != nullptr && hit->id() == kBaseButton,
           "an empty layer never shadows the base content");
}

void test_priority_inside_a_layer() {
    Fixture f;
    f.host->open_overlay(spec("high", OverlayLayer::Overlay, 20, 5));
    f.host->open_overlay(spec("low", OverlayLayer::Overlay, 21, 1));
    f.host->open_overlay(spec("high-later", OverlayLayer::Overlay, 22, 5));
    f.frame();
    Node* layer = f.host->overlays().layer_node(OverlayLayer::Overlay);
    expect(layer != nullptr && layer->children().size() == 3, "three entries in the layer");
    Node* hit = f.hit(10, 10);
    expect(hit != nullptr && hit->id() == 22,
           "higher priority paints above; equal priority keeps the open order");
    if (layer != nullptr && layer->children().size() == 3) {
        auto content_box = [&](std::size_t index) {
            // wrapper -> content -> box
            return layer->children()[index]->children()[0]->children()[0]->id();
        };
        expect(content_box(0) == 21 && content_box(1) == 20 && content_box(2) == 22,
               "entries are ordered by (priority, open order)");
    }
}

void test_handle_lifetime_and_close_if() {
    Fixture f;
    expect(!f.host->close_overlay(OverlayHandle{}), "an empty handle closes nothing");

    int mounts = 0;
    OverlaySpec never = spec("never", OverlayLayer::Overlay, 60);
    never.mount = [&mounts](Node&) { ++mounts; };
    const auto early = f.host->open_overlay(std::move(never));
    expect(f.host->close_overlay(early), "a pending entry can be closed");
    expect(!f.host->close_overlay(early), "closing twice reports false");
    f.frame();
    expect(mounts == 0, "closed before the boundary: it never mounts");
    expect(!f.host->overlays().is_open(early) && f.host->overlays().content(early) == nullptr &&
               f.host->overlays().size() == 0,
           "and leaves no entry behind");

    const auto first = f.host->open_overlay(spec("menu.a", OverlayLayer::Overlay, 61));
    f.host->open_overlay(spec("menu.b", OverlayLayer::Overlay, 62));
    f.host->open_overlay(spec("other", OverlayLayer::Toast, 63));
    f.frame();
    const std::size_t closed = f.host->overlays().close_if(
        [](const OverlayInfo& info) { return info.id.starts_with("menu."); });
    expect(closed == 2, "close_if reports what it closed");
    expect(f.host->overlays().size() == 1, "closing entries stop counting at once");
    f.frame();
    expect(!f.host->overlays().is_open(first), "a closed handle is no longer open");
    expect(!f.host->close_overlay(first), "a stale handle is a no-op");
    expect(f.host->overlays().layer_node(OverlayLayer::Overlay)->children().empty(),
           "the closed entries' nodes are gone");
    Node* hit = f.hit(10, 10);
    expect(hit != nullptr && hit->id() == 63, "the toast stays");
    expect(f.host->overlays().close_all() == 1, "close_all closes the rest");
    f.frame();
    expect(f.host->overlays().size() == 0, "nothing is left");
}

void test_reentrant_open_and_failure() {
    Fixture f;
    OverlaySpec outer = spec("outer", OverlayLayer::Overlay, 70);
    outer.mount = [&f](Node& content) {
        box(content, 70);
        f.host->overlays().open(spec("inner", OverlayLayer::Tooltip, 71));
    };
    f.host->open_overlay(std::move(outer));
    f.frame();
    Node* hit = f.hit(10, 10);
    expect(f.host->overlays().size() == 2 && hit != nullptr && hit->id() == 71,
           "an overlay opened from a mount callback lands in the same frame");

    OverlaySpec bad = spec("bad", OverlayLayer::Modal, 72);
    bad.mount = [](Node&) { throw std::runtime_error("content failed"); };
    f.host->open_overlay(std::move(bad));
    f.frame();
    expect(f.host->overlays().failed_count() == 1, "a throwing mount is counted");
    expect(f.host->overlays().layer_node(OverlayLayer::Modal) == nullptr,
           "and leaves nothing in the scene");
    expect(f.host->overlays().size() == 2, "the failed entry is dropped");
}

void test_modal_blocks_pointer_and_traps_focus() {
    Fixture f;
    InputRouter router;
    FocusManager& focus = router.focus_manager();
    focus.set_root(f.root);
    Node* base_button = f.root.children()[0]->children()[0].get();
    focus.request_focus(base_button);

    const auto dialog = f.host->open_overlay(spec("confirm", OverlayLayer::Modal, 50, 0, true));
    f.frame();
    expect(f.host->overlays().content(dialog) != nullptr &&
               f.host->overlays().top_modal() == dialog,
           "the dialog is up");
    Node* centre = f.hit(200, 400);
    expect(centre != nullptr && centre->id() == 50, "the dialog content takes its own hits");
    Node* outside = f.hit(10, 10);
    expect(outside != nullptr && outside->class_name() == "tessera-overlay-scrim",
           "everything else lands on the scrim, so the base is inert for the pointer");

    focus.sync_focus_trap(f.root);
    expect(focus.focused() != nullptr && focus.focused()->id() == 50,
           "focus moves into the dialog");
    expect(!focus.allows_focus(*base_button), "the base is not focusable behind it");
    InputEvent tab;
    tab.type = InputEventType::KeyDown;
    tab.key = key::kTab;
    router.dispatch(f.root, tab);
    expect(focus.focused() != nullptr && focus.focused()->id() == 50, "Tab stays inside");

    f.host->close_overlay(dialog);
    f.frame();
    focus.sync_focus_trap(f.root);
    expect(focus.focused() == base_button, "closing restores the focus held before");
    Node* hit = f.hit(10, 10);
    expect(hit != nullptr && hit->id() == kBaseButton, "and the base takes hits again");
}

void test_modal_semantics() {
    Fixture f;
    const auto dialog = f.host->open_overlay(spec("confirm", OverlayLayer::Modal, 50));
    f.host->open_overlay(spec("saved", OverlayLayer::Toast, 51));
    f.frame();
    a11y_test::StubStyles styles;
    tessera::ui::a11y::A11ySnapshot snapshot;
    const auto stats = a11y_test::build(f.root, styles, snapshot);
    expect(!has_id(snapshot, kBase) && !has_id(snapshot, kBaseButton),
           "the page behind a modal is hidden from the semantics tree");
    expect(stats.skipped_inert > 0, "and counted as inert");
    expect(has_id(snapshot, 50), "the dialog content is exposed");
    expect(has_id(snapshot, 51), "a toast above the dialog stays exposed");
    const Node* content = f.host->overlays().content(dialog);
    bool named = false;
    for (const auto& node : snapshot.nodes) {
        if (content != nullptr && node.id == content->id()) {
            named = node.role == tessera::ui::a11y::Role::Dialog &&
                    tessera::ui::a11y::name_of(node) == "confirm";
        }
    }
    expect(named, "the dialog is announced as a dialog with its name");

    f.host->close_overlay(dialog);
    f.frame();
    a11y_test::build(f.root, styles, snapshot);
    expect(has_id(snapshot, kBase), "closing the dialog brings the page back");
}

void test_top_modal_and_idle() {
    Fixture f;
    f.host->open_overlay(spec("a", OverlayLayer::Modal, 80, 1));
    const auto top = f.host->open_overlay(spec("b", OverlayLayer::Modal, 81, 2));
    f.host->open_overlay(spec("c", OverlayLayer::Modal, 82, 0));
    f.frame();
    expect(f.host->overlays().top_modal() == top, "the highest-priority modal is on top");
    f.frame();
    const auto idle = f.frame();
    expect(!idle.frame_due && !idle.tick.needs_layout, "a settled overlay scene is idle");
}

void test_host_destruction_detaches() {
    Node root(1);
    {
        OverlayHost host(root);
        host.open(spec("temp", OverlayLayer::Overlay, 90));
        host.flush();
        expect(root.children().size() == 1, "the layer is attached while the host lives");
    }
    expect(root.children().empty(), "destroying the host removes its layer nodes");
}

} // namespace

int main() {
    test_lazy_and_frame_boundary();
    test_layer_order_and_hit_testing();
    test_priority_inside_a_layer();
    test_handle_lifetime_and_close_if();
    test_reentrant_open_and_failure();
    test_modal_blocks_pointer_and_traps_focus();
    test_modal_semantics();
    test_top_modal_and_idle();
    test_host_destruction_detaches();
    if (failures != 0) {
        std::cerr << "overlay_host: " << failures << " failure(s)\n";
        return 1;
    }
    return 0;
}
