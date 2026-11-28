// SPDX-License-Identifier: MIT
//
// LiveCapsule component (issue #224, docs/specs/live-capsule-v1.md section 9, headless half): the
// capsule in the toast layer of a real AppHost, its semantics, focus rules, state parts, in-place
// update and idle behaviour. The unit half (placement, queue, morph) is live_capsule_tests.

#include "overlay_test_fixture.hpp"
#include "tessera/ui/capsule/live_capsule.hpp"
#include "tessera/ui/capsule/slot_measure.hpp"
#include "tessera/ui/text/text_measure.hpp"

#include <memory>
#include <string>

namespace {

using namespace overlay_test;
using namespace tessera::ui::capsule;
using tessera::ui::a11y::A11yNode;
using tessera::ui::a11y::A11ySnapshot;
using tessera::ui::a11y::name_of;
using tessera::ui::a11y::Role;
using tessera::ui::accessibility::AnnouncementQueue;

const Node* find_part(const Node& node, std::string_view part) {
    if (node.class_name() == part) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (const Node* found = find_part(*child, part)) {
            return found;
        }
    }
    return nullptr;
}

std::size_t count_parts(const Node& node, std::string_view part) {
    std::size_t count = node.class_name() == part ? 1 : 0;
    for (const auto& child : node.children()) {
        count += count_parts(*child, part);
    }
    return count;
}

LiveCapsuleEnv notch_env() {
    LiveCapsuleEnv env;
    env.window_width = 400.0f;
    env.window_height = 800.0f;
    env.top_inset = 24.0f;
    env.gesture_inset_top = 20.0f;
    env.cutouts.bounds[0] = {170.0f, 0.0f, 60.0f, 30.0f};
    env.cutouts.count = 1;
    return env;
}

// Mount, sync, settle: the overlay mounts at the frame boundary, so the first sync only opens it.
void settle(Fixture& f, LiveCapsule& capsule, const LiveCapsuleEnv& env) {
    capsule.sync(env, 0.0f);
    f.frame();
    for (int i = 0; i < 600; ++i) {
        const bool moving = capsule.sync(env, static_cast<float>(kFrame));
        f.frame();
        if (!moving) {
            break;
        }
    }
}

Activity timer(const char* id = "timer") {
    Activity a;
    a.id = id;
    a.label = "Timer running";
    a.slots.leading_width = 40.0f;
    a.slots.trailing_width = 40.0f;
    a.slots.title = "Timer";
    a.slots.actions.push_back({"Pause", {}});
    a.slots.actions.push_back({"Stop", {}});
    return a;
}

void test_semantics_placement_and_focus() {
    Fixture f;
    AnnouncementQueue announcements;
    LiveCapsuleHost host(&announcements);
    LiveCapsule capsule(host, f.host->overlays());
    const LiveCapsuleEnv env = notch_env();

    capsule.sync(env, 0.0f);
    expect(!capsule.handle() && f.host->overlays().size() == 0, "nothing queued: no overlay");

    FocusManager focus;
    focus.set_root(f.root);
    Node* base_button = f.root.children()[0]->children()[0].get();
    focus.request_focus(base_button);

    host.start(timer());
    settle(f, capsule, env);
    Node* content = f.host->overlays().content(capsule.handle());
    expect(content != nullptr, "the capsule mounts in the overlay host");
    expect(f.host->overlays().layer_node(OverlayLayer::Toast) != nullptr &&
               content->parent()->parent() == f.host->overlays().layer_node(OverlayLayer::Toast),
           "it lives in the toast layer");
    expect(focus.focused() == base_button, "starting an activity never changes the focused node");
    expect(announcements.size() == 1 && announcements.peek()->text == "Timer running",
           "start is announced once, politely");

    bool named = false;
    for (const auto& attribute : content->aria_attributes()) {
        named = named || (attribute.name == "aria-label" && attribute.value == "Timer running");
    }
    expect(named, "the accessible name comes from the front activity");
    expect(content->role() == "status", "role status");
    {
        A11ySnapshot snapshot;
        a11y_test::StubStyles styles;
        // The fixture does not run layout for overlay content, so the box the capsule wrote is
        // applied by hand; a zero-size node is not emitted.
        content->set_bounds(
            {capsule.rect().x, capsule.rect().y, capsule.rect().width, capsule.rect().height});
        (void)a11y_test::build(*f.host->overlays().layer_node(OverlayLayer::Toast), styles,
                               snapshot);
        const A11yNode* status = nullptr;
        for (const A11yNode& node : snapshot.nodes) {
            if (node.role == Role::Status) {
                status = &node;
            }
        }
        expect(status != nullptr, "the snapshot carries a Status node for the capsule");
        expect(status != nullptr && name_of(*status) == "Timer running",
               "the Status node is named from the front activity");
    }
    bool live = false;
    for (const auto& attribute : content->aria_attributes()) {
        live = live || (attribute.name == "aria-live" && attribute.value == "polite");
    }
    expect(live, "inside a polite live region");

    expect(capsule.state() == CapsuleState::Compact, "starts compact");
    const auto& rect = capsule.rect();
    expect(rect.width > 100.0f && rect.x + rect.width * 0.5f > 199.0f &&
               rect.x + rect.width * 0.5f < 201.0f,
           "centred on the notch");
    expect(count_parts(*content, "leading") == 1 && count_parts(*content, "trailing") == 1,
           "compact has the leading and trailing parts");

    // Tap expands; focus is remembered and handed back on collapse.
    expect(capsule.activate(base_button->id()) && capsule.state() == CapsuleState::Expanded,
           "tap expands");
    settle(f, capsule, env);
    content = f.host->overlays().content(capsule.handle());
    expect(find_part(*content, "card") != nullptr && count_parts(*content, "action") == 2,
           "expanded shows the card with two actions");
    const Node* title = find_part(*content, "title");
    expect(title != nullptr && !title->aria_attributes().empty(), "the title has a name");
    expect(focus.focused() == base_button, "expanding does not move focus either");
    expect(capsule.escape() && capsule.state() == CapsuleState::Compact, "Escape collapses");
    expect(capsule.take_focus_return() == base_button->id(), "focus returns to where it was");
    settle(f, capsule, env);
    content = f.host->overlays().content(capsule.handle());
    expect(find_part(*content, "card") == nullptr && find_part(*content, "leading") != nullptr,
           "back to the compact parts");

    // A press outside collapses; one inside does not.
    capsule.activate(0);
    expect(!capsule.pointer_down(rect.x + 5.0f, rect.y + 5.0f), "a press inside keeps it open");
    expect(capsule.pointer_down(5.0f, 700.0f) && capsule.state() == CapsuleState::Compact,
           "a press outside collapses");

    // End: the overlay closes and the end is announced.
    host.end("timer");
    capsule.sync(env, 0.0f);
    f.frame();
    expect(f.host->overlays().size() == 0 && !capsule.handle(),
           "ending the last activity closes it");
    expect(announcements.size() == 2, "the end is announced");
}

void test_update_in_place_and_idle() {
    Fixture f;
    LiveCapsuleHost host;
    LiveCapsule capsule(host, f.host->overlays());
    const LiveCapsuleEnv env = notch_env();
    int builds = 0;
    Activity a = timer();
    a.slots.leading = [&builds](Node&) { ++builds; };
    host.start(std::move(a));
    settle(f, capsule, env);
    const int after_start = builds;
    expect(after_start >= 1, "the leading slot built");

    const OverlayHandle handle = capsule.handle();
    Slots slots;
    slots.leading_width = 40.0f;
    slots.trailing_width = 40.0f;
    slots.leading = [&builds](Node&) { ++builds; };
    host.update("timer", std::move(slots));
    settle(f, capsule, env);
    expect(builds == after_start + 1, "update rebuilds the slots once");
    expect(capsule.handle() == handle && f.host->overlays().size() == 1,
           "in place: the same overlay, no second capsule");

    // Idle: nothing changed, nothing rebuilt, no frame due, the morph is not asking for ticks.
    const int idle_builds = builds;
    for (int i = 0; i < 10; ++i) {
        expect(!capsule.sync(env, static_cast<float>(kFrame)),
               "an idle capsule does not wake the pacer");
        const auto idle = f.frame();
        expect(!idle.frame_due, "an idle capsule presents nothing");
    }
    expect(builds == idle_builds, "an idle capsule rebuilds nothing");
}

void test_measured_slot_sizes() {
    // StubTextMeasurer advances 0.5 * font size per code point: "0:42" at 11px is 22 px wide.
    const auto stub = std::make_shared<tessera::ui::text::StubTextMeasurer>();
    const auto text = [](Node& slot, const char* value) {
        Node& node = slot.add_child(std::make_unique<Node>(tessera::ui::NodeId{9001}));
        node.set_element_kind("Text");
        tessera::ui::PaintData data;
        data.kind = tessera::ui::PaintKind::Text;
        data.text = value;
        node.set_paint_data(std::move(data));
    };
    const auto compact_width = [&](bool measured, const char* trailing) {
        Fixture f;
        LiveCapsuleHost host;
        LiveCapsule capsule(host, f.host->overlays());
        if (measured) {
            capsule.set_slot_measurer(make_intrinsic_slot_measurer(stub));
        }
        Activity a = timer();
        a.slots.leading_width = 100.0f;
        a.slots.trailing_width = 100.0f;
        a.slots.leading = [](Node& slot) {
            Node& dot = slot.add_child(std::make_unique<Node>(tessera::ui::NodeId{9002}));
            dot.set_element_kind("View");
            dot.set_inline_style("width", "14px");
            dot.set_inline_style("height", "14px");
        };
        a.slots.trailing = [&](Node& slot) { text(slot, trailing); };
        host.start(std::move(a));
        settle(f, capsule, notch_env());
        return capsule.rect().width;
    };
    // cutout 60 + twice the wider slot: trailing is 22 + 12, leading only 14 + 12.
    expect(std::abs(compact_width(true, "0:42") - 128.0f) < 0.5f,
           "compact width is the measured slots plus the cutout");
    expect(std::abs(compact_width(true, "0:42:17") - 161.0f) < 0.5f,
           "a longer trailing text widens the capsule");
    expect(std::abs(compact_width(false, "0:42") - 240.0f) < 0.5f,
           "without a measurer the hints apply");

    Fixture f;
    LiveCapsuleHost host;
    LiveCapsule capsule(host, f.host->overlays());
    capsule.set_slot_measurer(make_intrinsic_slot_measurer(stub));
    host.start(timer());
    const LiveCapsuleEnv env = notch_env();
    settle(f, capsule, env);
    capsule.activate(0);
    settle(f, capsule, env);
    // "Timer" 5 * 7 = 35 and two actions: (Pause 5*5.5 + 16 + 8) + (Stop 4*5.5 + 16 + 8) = 97.5.
    expect(std::abs(capsule.rect().width - (97.5f + 24.0f)) < 0.5f,
           "the expanded card is sized from its title, content and actions");
    expect(std::abs(capsule.rect().height - (20.0f + 24.0f + 24.0f)) < 0.5f,
           "the card height is title plus actions plus padding");
}

void test_reduced_motion_and_fallback() {
    Fixture f;
    LiveCapsuleHost host;
    LiveCapsule capsule(host, f.host->overlays());
    LiveCapsuleEnv env = notch_env();
    env.cutouts = {};
    env.reduced_motion = true;
    host.start(timer());
    settle(f, capsule, env);
    expect(std::abs(capsule.rect().y - 32.0f) < 0.01f &&
               std::abs(capsule.rect().x + capsule.rect().width * 0.5f - 200.0f) < 0.01f,
           "no cutout: top centre, 8 below the inset");
    capsule.activate(0);
    const bool moving = capsule.sync(env, 0.0f);
    expect(moving, "the cross-fade asks for ticks");
    settle(f, capsule, env);
    expect(capsule.rect().width > 200.0f && !capsule.active(), "reduced motion snaps the size");
}

// Rotation and fold changes (spec section 2): the position follows the window metrics on the next
// sync without rebuilding the content or reopening the overlay.
void test_rotation_and_fold_placement() {
    Fixture f;
    LiveCapsuleHost host;
    LiveCapsule capsule(host, f.host->overlays());
    const LiveCapsuleEnv portrait = notch_env();
    host.start(timer());
    settle(f, capsule, portrait);
    const OverlayHandle handle = capsule.handle();
    auto centre_x = [&] { return capsule.rect().x + capsule.rect().width * 0.5f; };
    expect(std::abs(centre_x() - 200.0f) < 0.01f && capsule.rect().y < 1.0f,
           "portrait: on the notch");

    // Rotate to landscape: the cutout is now on a side edge, so the capsule falls back to the
    // top-centre rule of the wider window.
    LiveCapsuleEnv landscape = portrait;
    landscape.window_width = 800.0f;
    landscape.window_height = 400.0f;
    landscape.top_inset = 0.0f;
    landscape.cutouts = {};
    landscape.cutouts.bounds[0] = {0.0f, 170.0f, 30.0f, 60.0f};
    landscape.cutouts.count = 1;
    settle(f, capsule, landscape);
    expect(std::abs(centre_x() - 400.0f) < 0.01f && std::abs(capsule.rect().y - 8.0f) < 0.01f,
           "landscape: top centre of the wider window, 8 below the inset");
    expect(capsule.handle() == handle && f.host->overlays().size() == 1,
           "rotation moves the capsule, it does not reopen it");

    // Unfold with a vertical hinge under the centre: the capsule moves to a pane.
    LiveCapsuleEnv folded = landscape;
    folded.hinge = {390.0f, 0.0f, 20.0f, 400.0f};
    settle(f, capsule, folded);
    const auto& rect = capsule.rect();
    expect(rect.x + rect.width <= 390.0f + 0.01f || rect.x >= 410.0f - 0.01f,
           "the capsule never sits under the hinge");

    // Fold closed: the hinge goes away and the capsule returns to the centre.
    settle(f, capsule, landscape);
    expect(std::abs(centre_x() - 400.0f) < 0.01f, "without the hinge it returns to the centre");

    // Back to portrait: onto the notch again.
    settle(f, capsule, portrait);
    expect(std::abs(centre_x() - 200.0f) < 0.01f && capsule.rect().y < 1.0f,
           "rotating back lands on the notch again");

    // The scene borrows inline-style text: after many rewrites the declarations must still read
    // the values the capsule last wrote (not freed or half-written memory).
    const Node* node = f.host->overlays().content(capsule.handle());
    std::string left;
    std::string width;
    for (const auto& declaration : node->inline_style()) {
        if (declaration.name == "left") {
            left = std::string(declaration.value);
        } else if (declaration.name == "width") {
            width = std::string(declaration.value);
        }
    }
    expect(left == std::to_string(std::lround(capsule.rect().x)) + "px" &&
               width == std::to_string(std::lround(capsule.rect().width)) + "px",
           "the box inline style owns its text");
}

void test_high_priority_preempts() {
    Fixture f;
    LiveCapsuleHost host;
    LiveCapsule capsule(host, f.host->overlays());
    const LiveCapsuleEnv env = notch_env();
    host.start(timer("a"));
    settle(f, capsule, env);
    capsule.activate(0);
    Activity call = timer("call");
    call.label = "Call";
    call.priority = Priority::High;
    host.start(std::move(call));
    settle(f, capsule, env);
    expect(capsule.state() == CapsuleState::Compact, "a new front starts compact, not expanded");
    expect(host.front()->id == "call" && host.find("a") != nullptr, "the previous one waits");
    host.end("call");
    settle(f, capsule, env);
    expect(host.front()->id == "a" && f.host->overlays().size() == 1, "and returns when it ends");
}

} // namespace

int main() {
    suite = "live_capsule_component";
    test_semantics_placement_and_focus();
    test_update_in_place_and_idle();
    test_measured_slot_sizes();
    test_reduced_motion_and_fallback();
    test_rotation_and_fold_placement();
    test_high_priority_preempts();
    if (failures != 0) {
        std::cerr << suite << ": " << failures << " failure(s)\n";
        return 1;
    }
    std::cout << suite << ": ok\n";
    return 0;
}
