// SPDX-License-Identifier: MIT

#include "tessera/ui/scene/input_dispatch.hpp"

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace {

using tessera::ui::Dirty;
using tessera::ui::Event;
using tessera::ui::EventPhase;
using tessera::ui::EventType;
using tessera::ui::has_flag;
using tessera::ui::InputEvent;
using tessera::ui::InputEventType;
using tessera::ui::InputRouter;
using tessera::ui::Node;
using tessera::ui::NodeId;
using tessera::ui::PointerKind;
using tessera::ui::style::has_state;
using tessera::ui::style::PseudoState;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "input_hover: " << message << "\n";
    }
    return condition;
}

const char* type_name(EventType type) {
    switch (type) {
    case EventType::PointerDown:
        return "down";
    case EventType::PointerUp:
        return "up";
    case EventType::PointerMove:
        return "move";
    case EventType::Wheel:
        return "wheel";
    case EventType::Text:
        return "text";
    case EventType::KeyDown:
        return "keydown";
    case EventType::KeyUp:
        return "keyup";
    case EventType::Focus:
        return "focus";
    case EventType::Blur:
        return "blur";
    case EventType::PointerOver:
        return "over";
    case EventType::PointerOut:
        return "out";
    case EventType::PointerEnter:
        return "enter";
    case EventType::PointerLeave:
        return "leave";
    case EventType::CompositionStart:
        return "composition-start";
    case EventType::CompositionUpdate:
        return "composition-update";
    case EventType::CompositionEnd:
        return "composition-end";
    case EventType::EditText:
        return "edit-text";
    }
    return "unknown";
}

InputEvent pointer(InputEventType type, float x, float y) {
    InputEvent event;
    event.type = type;
    event.x = x;
    event.y = y;
    return event;
}

InputEvent kind_pointer(InputEventType type, float x, float y, std::uint32_t pointer_id,
                        PointerKind kind) {
    InputEvent event = pointer(type, x, y);
    event.pointer_id = pointer_id;
    event.pointer_kind = kind;
    return event;
}

// Records boundary events in one shared trace so dispatch/chain order is assertable globally.
struct TraceNode : Node {
    TraceNode(NodeId id, std::vector<std::string>& trace)
        : Node(id),
          trace(&trace) {}

    void on_event(Event& event) override {
        const char* phase = event.phase == EventPhase::Target   ? "target"
                            : event.phase == EventPhase::Bubble ? "bubble"
                                                                : "capture";
        trace->push_back(std::to_string(id()) + ":" + type_name(event.type) + ":" + phase);
    }

    std::vector<std::string>* trace;
};

// Keeps only over/out/enter/leave entries; path assertions ignore the triggering pointer event.
std::vector<std::string> boundary_events(const std::vector<std::string>& trace) {
    std::vector<std::string> result;
    for (const std::string& entry : trace) {
        if (entry.find(":over:") != std::string::npos || entry.find(":out:") != std::string::npos ||
            entry.find(":enter:") != std::string::npos ||
            entry.find(":leave:") != std::string::npos) {
            result.push_back(entry);
        }
    }
    return result;
}

// Pointer boundary events: over/out bubble to the common ancestor exclusive, enter/leave fire once
// per node below it, capture freezes the chain, and touch never produces them.
bool pointer_boundary_events() {
    std::vector<std::string> trace;
    TraceNode root(30, trace);
    root.set_bounds({0, 0, 200, 200});
    auto panel_ptr = std::make_unique<TraceNode>(31, trace);
    panel_ptr->set_bounds({0, 0, 150, 150});
    TraceNode& panel = static_cast<TraceNode&>(root.add_child(std::move(panel_ptr)));
    auto a_ptr = std::make_unique<TraceNode>(32, trace);
    a_ptr->set_bounds({10, 10, 50, 50});
    TraceNode& a = static_cast<TraceNode&>(panel.add_child(std::move(a_ptr)));
    auto b_ptr = std::make_unique<TraceNode>(33, trace);
    b_ptr->set_bounds({70, 10, 50, 50});
    TraceNode& b = static_cast<TraceNode&>(panel.add_child(std::move(b_ptr)));
    InputRouter router;
    const auto hover = [&](float x, float y) {
        router.dispatch(root, pointer(InputEventType::PointerMove, x, y));
    };
    const std::vector<std::string> leave_all{"32:out:target",   "31:out:bubble",
                                             "30:out:bubble",   "32:leave:target",
                                             "31:leave:target", "30:leave:target"};
    const std::vector<std::string> a_to_b{"32:out:target", "32:leave:target", "33:over:target",
                                          "33:enter:target"};
    // Entering from nothing: over bubbles to the root, enter fires outermost -> innermost.
    hover(20, 20);
    if (!expect(boundary_events(trace) ==
                    std::vector<std::string>{"32:over:target", "31:over:bubble", "30:over:bubble",
                                             "30:enter:target", "31:enter:target",
                                             "32:enter:target"},
                "enter chain order") ||
        !expect(has_state(a.pseudo_state(), PseudoState::Hover), "hover bit on target") ||
        !expect(!has_state(panel.pseudo_state(), PseudoState::Hover), "no ancestor hover chain")) {
        return false;
    }

    // Same-parent move: out/leave the old target, over/enter the new one.
    trace.clear();
    hover(90, 20);
    if (!expect(boundary_events(trace) == a_to_b, "same-parent boundary order") ||
        !expect(!has_state(a.pseudo_state(), PseudoState::Hover), "old hover cleared") ||
        !expect(has_state(b.pseudo_state(), PseudoState::Hover), "new hover set") ||
        !expect(has_flag(b.dirty(), Dirty::Style), "hover marks Style dirty")) {
        return false;
    }

    // Moving to an ancestor leaves only the old child; moving back enters only the child.
    trace.clear();
    hover(5, 5);
    if (!expect(boundary_events(trace) ==
                    std::vector<std::string>{"33:out:target", "33:leave:target"},
                "ancestor move leaves only the child")) {
        return false;
    }
    trace.clear();
    hover(20, 20);
    if (!expect(boundary_events(trace) ==
                    std::vector<std::string>{"32:over:target", "32:enter:target"},
                "child move enters only the child")) {
        return false;
    }

    // Leaving the scene bubbles out through the whole chain, then leaves innermost -> outermost.
    trace.clear();
    hover(500, 500);
    if (!expect(boundary_events(trace) == leave_all, "scene leave chain order") ||
        !expect(router.hovered() == nullptr, "scene leave clears hover target")) {
        return false;
    }

    // Touch produces no boundary events and leaves the mouse hover chain alone.
    hover(20, 20);
    trace.clear();
    router.dispatch(root, kind_pointer(InputEventType::PointerDown, 20, 20, 5, PointerKind::Touch));
    router.dispatch(root, kind_pointer(InputEventType::PointerUp, 20, 20, 5, PointerKind::Touch));
    if (!expect(boundary_events(trace).empty(), "touch produces no boundary events") ||
        !expect(router.hovered() == &a, "touch leaves mouse hover alone")) {
        return false;
    }

    // Capture freezes the hover chain; the release resumes boundary events on the next move.
    router.dispatch(root, pointer(InputEventType::PointerDown, 20, 20));
    trace.clear();
    hover(90, 20);
    if (!expect(boundary_events(trace).empty(), "captured move fires no boundary events") ||
        !expect(router.hovered() == &a, "captured move keeps the hover chain")) {
        return false;
    }
    router.dispatch(root, pointer(InputEventType::PointerUp, 90, 20));
    trace.clear();
    hover(90, 20);
    if (!expect(boundary_events(trace) == a_to_b, "release resumes boundary events")) {
        return false;
    }

    // A primary mouse cancel and a window blur both leave no stale enter state.
    trace.clear();
    router.dispatch(root, pointer(InputEventType::PointerCancel, 90, 20));
    if (!expect(boundary_events(trace) ==
                    std::vector<std::string>{"33:out:target", "31:out:bubble", "30:out:bubble",
                                             "33:leave:target", "31:leave:target",
                                             "30:leave:target"},
                "cancel clears the hover chain") ||
        !expect(router.hovered() == nullptr, "cancel clears hover state")) {
        return false;
    }
    hover(20, 20);
    trace.clear();
    InputEvent blur;
    blur.type = InputEventType::Blur;
    router.dispatch(root, blur);
    return expect(boundary_events(trace) == leave_all, "blur clears the hover chain") &&
           expect(router.hovered() == nullptr, "blur leaves no stale enter state");
}
} // namespace

int main() {
    if (!pointer_boundary_events()) {
        return 1;
    }

    std::cout << "input_hover: ok\n";
    return 0;
}