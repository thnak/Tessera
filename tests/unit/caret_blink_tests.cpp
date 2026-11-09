// SPDX-License-Identifier: MIT
//
// CaretBlink timing/visibility state machine (task-67): phase boundaries, reset-on-activity,
// selection/unfocus/read-only hiding, repaint flags only at transitions, multi-period scripts,
// next-transition scheduling, the paint-decoration publish helper and determinism.

#include "tessera/ui/key_code.hpp"
#include "tessera/ui/text/caret_blink.hpp"
#include "tessera/ui/text/edit_controller.hpp"

#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using tessera::ui::Event;
using tessera::ui::EventType;
using tessera::ui::Modifier;
using tessera::ui::TextDecoration;
using tessera::ui::text::CaretBlink;
using tessera::ui::text::CaretBlinkConfig;
using tessera::ui::text::EditController;

namespace key = tessera::ui::key;

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "caret_blink: " << message << "\n";
    }
    return condition;
}

bool near(double lhs, double rhs) {
    return std::abs(lhs - rhs) <= 1.0e-9;
}

Event text_event(std::uint32_t codepoint) {
    Event event;
    event.type = EventType::Text;
    event.text_codepoint = codepoint;
    return event;
}

Event key_event(std::uint32_t key_code, Modifier modifiers = Modifier::None) {
    Event event;
    event.type = EventType::KeyDown;
    event.key = key_code;
    event.modifiers = modifiers;
    return event;
}

bool rejects(CaretBlinkConfig config) {
    try {
        CaretBlink blink(config);
        (void)blink;
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}

bool default_timing() {
    CaretBlink blink;
    if (!expect(blink.caret_visible(), "the caret starts visible") ||
        !expect(blink.active(), "the field starts active") ||
        !expect(near(blink.next_transition_seconds(), 0.5),
                "the first transition is at the visible boundary")) {
        return false;
    }
    if (!expect(!blink.tick(0.25), "no repaint inside the visible phase")) {
        return false;
    }
    // The visible window is half-open: exactly 0.5 s is already hidden.
    if (!expect(blink.tick(0.5), "the visible boundary reports a repaint") ||
        !expect(!blink.caret_visible(), "exactly visible_seconds is hidden") ||
        !expect(near(blink.next_transition_seconds(), 1.0),
                "the next transition is the hidden boundary")) {
        return false;
    }
    if (!expect(!blink.tick(0.75), "no repaint inside the hidden phase")) {
        return false;
    }
    if (!expect(blink.tick(1.0), "the period boundary reports a repaint") ||
        !expect(blink.caret_visible(), "exactly one period is visible again")) {
        return false;
    }
    return true;
}

bool boundary_and_jumps() {
    CaretBlink blink;
    if (!expect(!blink.tick(0.4999), "just before the boundary stays visible") ||
        !expect(blink.tick(0.5001), "just after the boundary is hidden")) {
        return false;
    }
    // A jump over one phase flips once; a jump over more phases reports the end state once.
    CaretBlink jumps;
    if (!expect(!jumps.tick(0.4), "jump: visible") ||
        !expect(jumps.tick(0.9), "jump across the visible boundary flips once") ||
        !expect(!jumps.tick(1.6), "a jump landing in the same phase does not flip") ||
        !expect(jumps.tick(2.1), "a jump landing in a different phase flips") ||
        !expect(!jumps.tick(2.1), "a repeated timestamp is a no-op")) {
        return false;
    }
    // Time never rewinds: a backwards tick is clamped, so it cannot replay transitions.
    CaretBlink monotonic;
    if (!expect(monotonic.tick(0.9), "monotonic: hidden after 0.9") ||
        !expect(!monotonic.tick(0.4), "a backwards tick is clamped (still hidden)") ||
        !expect(monotonic.caret_visible() == false, "monotonic: hidden after the boundary") ||
        !expect(monotonic.tick(1.1), "clamping does not lose the next transition") ||
        !expect(monotonic.caret_visible(), "monotonic: visible after 1.1")) {
        return false;
    }
    return true;
}

bool reset_on_activity() {
    CaretBlink blink;
    if (!expect(!blink.tick(0.4), "visible before activity") ||
        !expect(!blink.reset(0.4), "activity while visible needs no repaint") ||
        !expect(!blink.tick(0.8), "the cycle restarted: still visible at 0.8") ||
        !expect(blink.tick(0.9), "the restarted cycle hides at 0.9") ||
        !expect(!blink.caret_visible(), "hidden after the restarted cycle")) {
        return false;
    }
    // Activity while hidden shows the caret immediately and restarts the cycle.
    if (!expect(blink.reset(0.95), "activity while hidden reports a repaint") ||
        !expect(blink.caret_visible(), "activity while hidden shows the caret") ||
        !expect(!blink.tick(1.4), "the restarted cycle stays visible at 1.4") ||
        !expect(blink.tick(1.45), "the restarted cycle hides at 1.45")) {
        return false;
    }
    return true;
}

bool selection_and_focus_hide_caret() {
    CaretBlink blink;
    if (!expect(blink.set_active(false, 0.1),
                "deactivating hides the caret and reports a repaint") ||
        !expect(!blink.caret_visible(), "inactive field hides the caret") ||
        !expect(!blink.tick(0.6), "inactive field never repaints") ||
        !expect(!blink.tick(5.0), "inactive field stays hidden over many periods") ||
        !expect(std::isinf(blink.next_transition_seconds()),
                "inactive field schedules no transition") ||
        !expect(!blink.set_active(false, 5.0), "re-deactivating is a no-op")) {
        return false;
    }
    // Reactivating shows the caret immediately and restarts the cycle.
    if (!expect(blink.set_active(true, 5.0), "reactivating reports a repaint") ||
        !expect(blink.caret_visible(), "reactivating shows the caret") ||
        !expect(!blink.tick(5.4), "the reactivated cycle is visible at 5.4") ||
        !expect(blink.tick(5.5), "the reactivated cycle hides at 5.5") ||
        !expect(!blink.set_active(true, 6.0), "re-activating an active field is a no-op")) {
        return false;
    }
    // Resetting an inactive field must not show the caret.
    blink.set_active(false, 6.0);
    return expect(!blink.reset(6.1), "reset on an inactive field needs no repaint") &&
           expect(!blink.caret_visible(), "reset on an inactive field keeps it hidden");
}

bool repaint_only_at_transitions() {
    CaretBlink blink;
    int changes = 0;
    // Dyadic steps land exactly on the 0.5 s boundaries of a 1 s period.
    for (int index = 1; index <= 20; ++index) {
        if (blink.tick(static_cast<double>(index) * 0.125)) {
            ++changes;
        }
    }
    if (!expect(changes == 5, "2.5 periods produce exactly five transitions (2 per period)") ||
        !expect(!blink.caret_visible(), "2.5 s is in the hidden phase of the third period")) {
        return false;
    }
    // A dense script over several periods: no flags between boundaries.
    CaretBlink dense;
    int dense_changes = 0;
    for (int index = 1; index <= 200; ++index) {
        const double now = static_cast<double>(index) * 0.01;
        const bool changed = dense.tick(now);
        dense_changes += changed ? 1 : 0;
        const double phase = now - std::floor(now);
        const bool boundary = std::abs(phase) < 1.0e-9 || std::abs(phase - 0.5) < 1.0e-9;
        if (changed && !boundary) {
            return expect(false, "a repaint flag appeared between phase boundaries");
        }
    }
    return expect(dense_changes == 4, "2 s of 10 ms ticks produce four transitions");
}

bool custom_phases() {
    CaretBlink blink(CaretBlinkConfig{0.25, 0.25});
    if (!expect(!blink.tick(0.24), "custom: visible before 0.25") ||
        !expect(blink.tick(0.25), "custom: hidden at exactly 0.25") ||
        !expect(blink.tick(0.5), "custom: visible at exactly 0.5 (period 0.5)") ||
        !expect(blink.tick(0.75), "custom: hidden again at 0.75") ||
        !expect(!rejects(CaretBlinkConfig{0.25, 0.25}), "custom config is accepted")) {
        return false;
    }
    return expect(rejects(CaretBlinkConfig{0.0, 0.5}), "zero visible phase rejected") &&
           expect(rejects(CaretBlinkConfig{0.5, 0.0}), "zero hidden phase rejected") &&
           expect(rejects(CaretBlinkConfig{-1.0, 0.5}), "negative phase rejected") &&
           expect(rejects(CaretBlinkConfig{std::numeric_limits<double>::infinity(), 0.5}),
                  "infinite phase rejected");
}

bool publishes_into_paint_decoration() {
    CaretBlink blink;
    TextDecoration decoration;
    decoration.caret_byte_offset = 3;
    decoration.selection_begin = 3;
    decoration.selection_end = 3;
    tessera::ui::text::publish_caret_visibility(decoration, blink);
    if (!expect(decoration.caret_visible, "publish: visible maps to caret_visible") ||
        !expect(near(decoration.blink_phase, 1.0), "publish: visible maps to phase 1") ||
        !expect(decoration.caret_visible && decoration.blink_phase >= 0.5f,
                "the paint visibility rule sees a visible caret")) {
        return false;
    }
    blink.tick(0.5);
    tessera::ui::text::publish_caret_visibility(decoration, blink);
    if (!expect(!decoration.caret_visible, "publish: hidden maps to caret_visible=false") ||
        !expect(near(decoration.blink_phase, 0.0), "publish: hidden maps to phase 0") ||
        !expect(!(decoration.caret_visible && decoration.blink_phase >= 0.5f),
                "the paint visibility rule hides the caret")) {
        return false;
    }
    // Hiding must not move the caret/selection bytes: blink is visibility only.
    return expect(decoration.caret_byte_offset == 3 && decoration.selection_begin == 3 &&
                      decoration.selection_end == 3,
                  "blink only changes visibility, not caret/selection offsets");
}

bool controller_activity_and_policy() {
    EditController controller;
    CaretBlink blink;
    if (!expect(controller.activity_count() == 0, "no activity on a fresh controller") ||
        !expect(controller.caret_blink_active(true), "collapsed focused editable field blinks")) {
        return false;
    }
    Event insert = text_event('A');
    if (!expect(controller.handle_event(insert), "text event consumed") ||
        !expect(controller.activity_count() == 1, "typing counts as activity")) {
        return false;
    }
    // Activity resets the blink and keeps it visible.
    if (!expect(!blink.reset(0.4), "typing while visible needs no repaint") ||
        !expect(!blink.tick(0.8), "typing restarted the blink cycle")) {
        return false;
    }
    Event select_all = key_event('A', Modifier::Control);
    if (!expect(controller.handle_event(select_all), "select-all consumed") ||
        !expect(controller.activity_count() == 2, "selection change counts as activity") ||
        !expect(controller.buffer().has_selection(), "a selection exists") ||
        !expect(!controller.caret_blink_active(true),
                "a non-collapsed selection hides the caret (platform-typical)")) {
        return false;
    }
    if (!expect(blink.set_active(false, 0.9), "selection hides the caret") ||
        !expect(!blink.caret_visible(), "selection: caret hidden")) {
        return false;
    }
    // Collapsing the selection restores the caret immediately.
    Event move_left = key_event(key::kArrowLeft);
    if (!expect(controller.handle_event(move_left), "arrow consumed") ||
        !expect(!controller.buffer().has_selection(), "selection collapsed") ||
        !expect(controller.caret_blink_active(true), "collapsed focused field blinks again") ||
        !expect(blink.set_active(true, 1.0), "collapsing the selection shows the caret")) {
        return false;
    }
    if (!expect(blink.caret_visible(), "collapsing shows the caret immediately")) {
        return false;
    }
    // Read-only and unfocused fields keep the caret hidden.
    controller.set_read_only(true);
    if (!expect(!controller.caret_blink_active(true), "read-only hides the caret")) {
        return false;
    }
    controller.set_read_only(false);
    return expect(!controller.caret_blink_active(false), "unfocused hides the caret") &&
           expect(controller.caret_blink_active(true), "focused editable still blinks");
}

bool determinism() {
    const auto run = [] {
        CaretBlink blink;
        std::vector<bool> visibility;
        std::vector<bool> changes;
        for (int index = 0; index <= 300; ++index) {
            const double now = static_cast<double>(index) * 0.01;
            if (index == 40 || index == 150) {
                blink.reset(now);
            }
            if (index == 100) {
                blink.set_active(false, now);
            }
            if (index == 120) {
                blink.set_active(true, now);
            }
            changes.push_back(blink.tick(now));
            visibility.push_back(blink.caret_visible());
        }
        return std::pair{visibility, changes};
    };
    return expect(run() == run(), "the same script gives identical visibility/repaint sequences");
}

} // namespace

int main() {
    if (!default_timing()) {
        return 1;
    }
    if (!boundary_and_jumps()) {
        return 2;
    }
    if (!reset_on_activity()) {
        return 3;
    }
    if (!selection_and_focus_hide_caret()) {
        return 4;
    }
    if (!repaint_only_at_transitions()) {
        return 5;
    }
    if (!custom_phases()) {
        return 6;
    }
    if (!publishes_into_paint_decoration()) {
        return 7;
    }
    if (!controller_activity_and_policy()) {
        return 8;
    }
    if (!determinism()) {
        return 9;
    }

    std::cout << "caret_blink: ok\n";
    return 0;
}
