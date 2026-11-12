// SPDX-License-Identifier: MIT

#include "tessera/automation/engine_host.hpp"
#include "tessera/diagnostics/trace.hpp"
#include "tessera/ui/scene/focus.hpp"
#include "tessera/ui/scene/scroll.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace tessera::automation {
namespace {

namespace harness = tessera::harness;
namespace ui = tessera::ui;

// One event per fixed simulation step, so timestamps never depend on a wall clock.
constexpr double kEventStepSeconds = 1.0 / 60.0;

harness::ProtocolError error(harness::ProtocolErrorCode code, std::string message,
                             std::string detail = {}) {
    return harness::ProtocolError{code, std::move(message), std::move(detail), std::nullopt};
}

std::unexpected<harness::ProtocolError> unexpected(harness::ProtocolError value) {
    return std::unexpected(std::move(value));
}

ui::Node* find_visible(ui::Node& node, harness::UiId ui_id) {
    if (!node.visible()) {
        return nullptr;
    }
    if (node.automation_id() == ui_id) {
        return &node;
    }
    for (const auto& child : node.children()) {
        if (ui::Node* found = find_visible(*child, ui_id); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

ui::InputEvent pointer_event(ui::InputEventType type, const ui::rhi::Rect& bounds,
                             double timestamp) {
    ui::InputEvent event;
    event.type = type;
    event.timestamp_s = timestamp;
    event.x = bounds.x + bounds.width * 0.5f;
    event.y = bounds.y + bounds.height * 0.5f;
    event.pointer_id = 0;
    event.pointer_kind = ui::PointerKind::Mouse;
    event.button = ui::PointerButton::Left;
    event.is_primary = true;
    return event;
}

// Largest prefix of `text` that fits `max_bytes` and ends on a UTF-8 sequence boundary. The script
// parser already guarantees valid UTF-8; this only splits it for the 256-byte inline event payload.
std::size_t chunk_length(std::string_view text, std::size_t offset, std::size_t max_bytes) {
    std::size_t length = std::min(max_bytes, text.size() - offset);
    while (length > 0 && offset + length < text.size() &&
           (static_cast<unsigned char>(text[offset + length]) & 0xC0U) == 0x80U) {
        --length;
    }
    return length;
}

bool finite_bounds(const ui::rhi::Rect& bounds) {
    return std::isfinite(bounds.x) && std::isfinite(bounds.y) && std::isfinite(bounds.width) &&
           std::isfinite(bounds.height);
}

} // namespace

EngineAutomationHost::EngineAutomationHost(ui::Node& root, app::AppHost& host,
                                           runtime::RuntimeLoop& loop, Config config)
    : root_(root),
      host_(host),
      loop_(loop),
      config_(config) {
    if (config_.max_snapshot_nodes == 0) {
        config_.max_snapshot_nodes = kMaxSnapshotNodes;
    }
}

void EngineAutomationHost::set_diagnostics(AutomationDiagnostics diagnostics) {
    config_.diagnostics = diagnostics;
}

const AutomationDiagnostics& EngineAutomationHost::diagnostics() const noexcept {
    return config_.diagnostics;
}

void EngineAutomationHost::record(std::string_view name,
                                  diagnostics::TraceCategory category) const {
    if (!diagnostics_enabled() || config_.diagnostics.trace == nullptr) {
        return;
    }
    config_.diagnostics.trace->instant(name, category);
    ++stats_.trace_events;
}

double EngineAutomationHost::next_event_seconds() {
    const double timestamp = static_cast<double>(event_count_) * kEventStepSeconds;
    ++event_count_;
    return timestamp;
}

ui::Node* EngineAutomationHost::find_target(harness::UiId ui_id) const {
    if (ui_id == 0) {
        return nullptr; // 0 means "no automation id"; it is never a target
    }
    return find_visible(root_, ui_id);
}

void EngineAutomationHost::settle() {
    if (!host_.frame_due()) {
        return;
    }
    // One settle frame, zero dt: state changed by input reaches style/layout/paint exactly once,
    // and the caller's next tick sees a clean tree.
    (void)host_.begin_frame(0.0);
    host_.end_frame();
    ++stats_.settle_frames;
}

std::expected<void, harness::ProtocolError>
EngineAutomationHost::dispatch_action(const harness::ActionCommand& action) {
    ++stats_.actions;
    const auto resolved = resolve_target(action.ui_id, action.item_key);
    if (!resolved.has_value()) {
        return std::unexpected(resolved.error());
    }
    ui::Node* target = *resolved;
    switch (action.kind) {
    case harness::ActionKind::Click:
        return click(*target);
    case harness::ActionKind::SetValue:
        return type_text(*target, action.value);
    case harness::ActionKind::Focus:
        return focus(*target);
    case harness::ActionKind::Scroll:
        return scroll(*target, action.scroll_dx, action.scroll_dy);
    }
    return unexpected(error(harness::ProtocolErrorCode::InvalidArgument, "unknown action kind"));
}

std::expected<void, harness::ProtocolError> EngineAutomationHost::click(ui::Node& target) {
    const auto bounds = target.world_bounds();
    if (!finite_bounds(bounds)) {
        return unexpected(error(harness::ProtocolErrorCode::Internal,
                                "click target has non-finite bounds", "bounds"));
    }
    const ui::InputEvent move =
        pointer_event(ui::InputEventType::PointerMove, bounds, next_event_seconds());
    const ui::InputEvent down =
        pointer_event(ui::InputEventType::PointerDown, bounds, next_event_seconds());
    const ui::InputEvent up =
        pointer_event(ui::InputEventType::PointerUp, bounds, next_event_seconds());
    // The whole gesture goes through the pump's bounded queue, exactly like platform input: hover,
    // implicit capture, focus and click synthesis are the production code paths.
    if (!pump_.queue().push(move) || !pump_.queue().push(down) || !pump_.queue().push(up)) {
        return unexpected(
            error(harness::ProtocolErrorCode::Internal, "input queue is full", "click"));
    }
    (void)pump_.pump(root_, loop_);
    settle();
    record("automation.click", diagnostics::TraceCategory::Input);
    return {};
}

std::expected<void, harness::ProtocolError>
EngineAutomationHost::type_text(ui::Node& target, const std::string& text) {
    if (!target.focusable()) {
        return unexpected(error(harness::ProtocolErrorCode::InvalidArgument,
                                "type target is not focusable",
                                std::to_string(target.automation_id())));
    }
    // Programmatic focus uses the same FocusManager the input router drives; the committed text
    // then reaches the focused node as EditText through the pump.
    if (!pump_.router().focus_manager().request_focus(&target)) {
        return unexpected(error(harness::ProtocolErrorCode::InvalidArgument,
                                "type target cannot take focus",
                                std::to_string(target.automation_id())));
    }
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t length = chunk_length(text, offset, ui::kMaxEditTextBytes);
        if (length == 0) {
            return unexpected(error(harness::ProtocolErrorCode::Internal,
                                    "typed text is not valid UTF-8", "text"));
        }
        ui::InputEvent event;
        event.type = ui::InputEventType::EditText;
        event.timestamp_s = next_event_seconds();
        std::copy_n(text.data() + offset, length, event.text.begin());
        event.text_size = static_cast<std::uint32_t>(length);
        event.text_truncated = offset + length < text.size();
        if (!pump_.queue().push(event)) {
            return unexpected(
                error(harness::ProtocolErrorCode::Internal, "input queue is full", "type"));
        }
        offset += length;
        if (pump_.queue().size() == pump_.queue().capacity()) {
            (void)pump_.pump(root_, loop_);
        }
    }
    if (offset != 0) {
        (void)pump_.pump(root_, loop_);
    }
    settle();
    record("automation.type", diagnostics::TraceCategory::Input);
    return {};
}

std::expected<void, harness::ProtocolError> EngineAutomationHost::focus(ui::Node& target) {
    if (!target.focusable() || !pump_.router().focus_manager().request_focus(&target)) {
        return unexpected(error(harness::ProtocolErrorCode::InvalidArgument,
                                "focus target cannot take focus",
                                std::to_string(target.automation_id())));
    }
    settle();
    record("automation.focus", diagnostics::TraceCategory::Input);
    return {};
}

std::expected<void, harness::ProtocolError> EngineAutomationHost::scroll(ui::Node& target,
                                                                         double dx, double dy) {
    if (!std::isfinite(dx) || !std::isfinite(dy)) {
        return unexpected(error(harness::ProtocolErrorCode::InvalidArgument,
                                "scroll offsets must be finite", "dx/dy"));
    }
    const auto bounds = target.world_bounds();
    if (!finite_bounds(bounds)) {
        return unexpected(error(harness::ProtocolErrorCode::Internal,
                                "scroll target has non-finite bounds", "bounds"));
    }
    // Wheel routing converts platform units to logical pixels (kPixelsPerWheelUnit), so a script's
    // pixel delta is carried as the equivalent wheel unit. The scroll model then clamps it exactly
    // like a real wheel event.
    ui::InputEvent event;
    event.type = ui::InputEventType::Wheel;
    event.timestamp_s = next_event_seconds();
    event.x = bounds.x + bounds.width * 0.5f;
    event.y = bounds.y + bounds.height * 0.5f;
    event.is_primary = true;
    event.dx = static_cast<float>(dx / ui::kPixelsPerWheelUnit);
    event.dy = static_cast<float>(dy / ui::kPixelsPerWheelUnit);
    if (!pump_.queue().push(event)) {
        return unexpected(
            error(harness::ProtocolErrorCode::Internal, "input queue is full", "scroll"));
    }
    (void)pump_.pump(root_, loop_);
    settle();
    record("automation.scroll", diagnostics::TraceCategory::Input);
    return {};
}

} // namespace tessera::automation
