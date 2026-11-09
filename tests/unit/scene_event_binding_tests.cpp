// SPDX-License-Identifier: MIT

// Scene event payload records and generated event bindings (docs/design/scene-graph.md section 6):
// field mapping, the per-node binding cap, Target-phase ordering, read-only bindings and route-log
// equality when no binding is installed. No GPU.

#include "tessera/ui/scene/event.hpp"
#include "tessera/ui/scene/event_payloads.hpp"
#include "tessera/ui/scene/node.hpp"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

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

std::size_t index_of(const std::vector<std::string>& log, std::string_view entry) {
    for (std::size_t i = 0; i < log.size(); ++i) {
        if (log[i] == entry) {
            return i;
        }
    }
    return log.size();
}

void test_payload_mapping() {
    ui::Event event;
    event.type = ui::EventType::PointerUp;
    event.x = 4.0f;
    event.y = 7.5f;
    event.pointer_id = 3;
    event.pointer_kind = ui::PointerKind::Pen;
    event.button = ui::PointerButton::Right;
    event.pressure = 0.25f;
    event.timestamp_s = 12.5;
    event.is_primary = true;
    event.modifiers = ui::Modifier::Shift | ui::Modifier::Control;

    const ui::PointerEventInfo pointer = ui::pointer_event_info(event);
    check(pointer.x == 4.0f && pointer.y == 7.5f, "pointer position maps");
    check(pointer.pointer_id == 3, "pointer id maps");
    check(pointer.pointer_kind == ui::PointerKind::Pen, "pointer kind maps");
    check(pointer.button == ui::PointerButton::Right, "pointer button maps");
    check(pointer.pressure == 0.25f && pointer.timestamp_s == 12.5, "pressure/timestamp map");
    check(pointer.is_primary, "primary flag maps");
    check(pointer.shift && pointer.ctrl && !pointer.alt && !pointer.meta, "modifiers map");

    const ui::ClickEvent click = ui::click_event(event);
    check(click.x == 4.0f && click.y == 7.5f && click.button == ui::PointerButton::Right,
          "click maps the pointer fields");
    check(click.shift && click.ctrl, "click modifiers map");

    event.key = 0x1B;
    event.repeat = true;
    const ui::KeyEventInfo key = ui::key_event_info(event);
    check(key.key == 0x1B && key.code == 0, "key codepoint maps; physical code is not on Event");
    check(key.repeat, "key repeat maps");

    constexpr std::string_view kValue = "hello";
    event.text_size = static_cast<std::uint32_t>(kValue.size());
    for (std::size_t i = 0; i < kValue.size(); ++i) {
        event.text[i] = kValue[i];
    }
    event.selection_start = 1;
    event.selection_end = 4;
    const ui::TextChangeEvent change = ui::text_change_event(event);
    check_eq(change.value, kValue, "text value borrows the event payload");
    check_eq(change.target_value, kValue, "target.value equals value");
    check(change.selection_start == 1 && change.selection_end == 4, "selection maps");
    check_eq(ui::text_submit_event(event).value, kValue, "submit uses the text change record");

    event.scroll_x = -2.0f;
    event.scroll_y = 9.0f;
    const ui::ScrollEventInfo scroll = ui::scroll_event_info(event);
    check(scroll.offset_x == -2.0f && scroll.offset_y == 9.0f, "scroll offsets map");

    const ui::FocusEventInfo focus = ui::focus_event_info(event);
    (void)focus;
    check_eq(ui::error_info(event).code, "", "error info is empty until the boundary slice");
}

void test_binding_capacity() {
    const auto handler = [](void*, const ui::Event&) {};
    ui::Node node(1);
    check(!node.bind_event(ui::EventType::PointerUp, nullptr), "a null handler is rejected");
    for (std::size_t i = 0; i < ui::Node::kMaxEventBindings; ++i) {
        check(node.bind_event(ui::EventType::PointerUp, handler),
              "bindings below the cap are accepted");
    }
    check(!node.bind_event(ui::EventType::KeyDown, handler),
          "a binding beyond the cap is rejected");
    check(node.event_bindings().size() == ui::Node::kMaxEventBindings, "the cap is stored");
    check(node.has_event_binding(ui::EventType::PointerUp), "has_event_binding sees the type");
    check(!node.has_event_binding(ui::EventType::KeyDown), "an unbound type is not reported");

    check(node.unbind_event(ui::EventType::PointerUp), "unbinding an installed type succeeds");
    check(!node.unbind_event(ui::EventType::KeyDown), "unbinding an absent type returns false");
    check(node.event_bindings().size() == ui::Node::kMaxEventBindings - 1,
          "unbind shrinks storage");
    node.clear_event_bindings();
    check(node.event_bindings().empty(), "clearing removes every binding");
    check(node.dirty() == ui::Dirty::None, "binding never marks a dirty domain");
}

// A node that writes every dispatched phase into a shared log so the route can be compared.
class Recorder : public ui::Node {
public:
    Recorder(ui::NodeId id, std::vector<std::string>* log)
        : ui::Node(id),
          log_(log) {}

    void on_event(ui::Event& event) override {
        const std::string_view phase = event.phase == ui::EventPhase::Capture  ? "capture"
                                       : event.phase == ui::EventPhase::Target ? "target"
                                                                               : "bubble";
        log_->push_back(std::to_string(id()) + ":" + std::string(phase));
    }

private:
    std::vector<std::string>* log_;
};

struct Route {
    std::vector<std::string> log;
    ui::Event event;
};

Route dispatch(bool bind) {
    Route route;
    auto root = std::make_unique<Recorder>(1, &route.log);
    auto target = std::make_unique<Recorder>(2, &route.log);
    Recorder& target_ref = *target;
    root->add_child(std::move(target));
    if (bind) {
        check(target_ref.bind_event(
                  ui::EventType::PointerUp,
                  [](void* context, const ui::Event&) {
                      static_cast<std::vector<std::string>*>(context)->push_back("binding");
                  },
                  &route.log),
              "the test binding installs");
    }
    route.event.type = ui::EventType::PointerUp;
    ui::dispatch_event_to(target_ref, route.event);
    return route;
}

void test_dispatch_contract() {
    const Route never_bound = dispatch(false);
    check(never_bound.log.size() == 3, "capture, target and bubble run for a binding-less node");
    if (never_bound.log.size() == 3) {
        check_eq(never_bound.log[0], "1:capture", "parent capture phase");
        check_eq(never_bound.log[1], "2:target", "target phase");
        check_eq(never_bound.log[2], "1:bubble", "parent bubble phase");
    }

    const Route bound = dispatch(true);
    check(bound.log.size() == 4, "the binding adds exactly one target-phase invocation");
    check(index_of(bound.log, "2:target") < index_of(bound.log, "binding"),
          "the binding runs after the node's own target listener");
    check(index_of(bound.log, "binding") < index_of(bound.log, "1:bubble"),
          "the binding runs before bubbling");
    check(!bound.event.handled, "a read-only binding cannot consume the event");

    // The route log is unchanged when bindings are cleared: the feature is dormant without one.
    const Route cleared = dispatch(false);
    check(cleared.log == never_bound.log, "the route log matches when no binding is installed");
}

} // namespace

int main() {
    test_payload_mapping();
    test_binding_capacity();
    test_dispatch_contract();
    if (failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }
    std::cout << "scene event binding tests passed\n";
    return 0;
}
