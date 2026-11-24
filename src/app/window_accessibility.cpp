// SPDX-License-Identifier: MIT
//
// WindowHost's accessibility wiring (issues #185 and #186, docs/specs/accessibility-v1.md section
// 12.3). This file is deliberately separate from window.cpp: it adapts the scene's paint style
// provider to the semantics builder's provider, supplies the snapshot source the controller builds
// from, and executes the drained actions on the scene (focus/press). window.cpp only constructs,
// ticks and tears the controller down, so the driver's own translation unit adds a handful of
// lines.

#include "tessera/ui/scene/node.hpp"
#include "window_impl.hpp"

#include <utility>

namespace tessera::app {
namespace {

namespace a11y = tessera::ui::a11y;

// Forwards the host's computed-style provider (the paint face of the same seam) to the
// accessibility
// builder's provider. One line of behaviour: the a11y builder declares the provider shape itself
// so
// the accessibility layer does not depend on paint/render, and this adapter is the join.
class PaintToA11yStyles final : public a11y::StyleProvider {
public:
    explicit PaintToA11yStyles(ui::paint::StyleProvider& styles)
        : styles_(styles) {}

    [[nodiscard]] ui::style::ComputedStyle compute(const ui::Node& node) override {
        return styles_.compute(node);
    }

private:
    ui::paint::StyleProvider& styles_;
};

// Depth-first lookup by stable node id. The scene is bounded (kMaxNodes in the snapshot), and this
// only runs for an action that assistive technology already validated against the snapshot.
ui::Node* find_node_by_id(ui::Node& node, ui::NodeId id) {
    if (node.id() == id) {
        return &node;
    }
    for (const std::unique_ptr<ui::Node>& child : node.children()) {
        if (ui::Node* found = find_node_by_id(*child, id); found != nullptr) {
            return found;
        }
    }
    return nullptr;
}

} // namespace

void WindowHost::Impl::start_accessibility() {
    // The controller lives in `accessibility` and is declared before `window` in window_impl.hpp,
    // so
    // it is destroyed first: its destructor uninstalls the UIA root and joins the bridge/session
    // while the platform window (and its native handle) is still valid.
    if (window == nullptr || host == nullptr) {
        return;
    }
    accessibility = std::make_unique<AccessibilityHost>();
    AccessibilityHostConfig config;
    // A shell that installed a pull-side provider wants that backend: its platform asks for one
    // node at a time and has no bridge to push a tree to. Every other platform leaves this null and
    // keeps the push bridge.
    if (options.accessibility_provider != nullptr) {
        config.backend = AccessibilityBackend::Provider;
        config.provider = options.accessibility_provider;
    }
    (void)accessibility->start(
        *window,
        [this](a11y::A11ySnapshot& out) {
            a11y::BuildOptions options_for_build;
            ui::FocusManager& focus = pump.router().focus_manager();
            if (focus.focused() != nullptr) {
                options_for_build.focused_id = focus.focused()->id();
            }
            PaintToA11yStyles styles(host->styles());
            (void)a11y::build_a11y_tree(root, styles, out, options_for_build);
        },
        [this](const AccessibilityAction& action) { apply_accessibility_action(action); }, config);
}

void WindowHost::Impl::tick_accessibility(const ui::frame::FrameTick* frame) {
    if (accessibility == nullptr) {
        return;
    }
    // A style or layout pass can change roles, names, states and bounds without touching anything
    // the driver watches separately, so it is a semantics change. The controller itself is
    // dirty-only, so a static window still does no build, no publish and no bridge work.
    if (frame != nullptr && (frame->needs_style || frame->needs_layout)) {
        accessibility->mark_dirty();
    }
    (void)accessibility->tick();
}

// One `TextChange` carrying `text`, dispatched into `target`.
//
// This mirrors `TextInputRuntime::emit_change` (the app's own path for a committed edit): the event
// borrows the caller's storage for the duration of the dispatch, and `text` outlives this call
// because it lives in the drained action, so no copy is needed. The scene does not own the buffer,
// so the runtime that DOES own it adopts the value through its bound `TextChange` handler - which
// is exactly how a screen reader's `SetText` reaches the same edit path the keyboard uses.
void WindowHost::Impl::dispatch_text_value(ui::Node& target, std::string_view text) {
    ui::Event event;
    event.type = ui::EventType::TextChange;
    event.text_view = text.data();
    event.text_view_size = static_cast<std::uint32_t>(text.size());
    event.set_text(text);
    // A whole-buffer replacement leaves the caret at the end, which is where a screen reader that
    // just typed into the field expects it.
    event.selection_start = static_cast<std::uint32_t>(text.size());
    event.selection_end = event.selection_start;
    (void)ui::dispatch_event_to(target, event);
    loop->mark_render_dirty();
}

void WindowHost::Impl::apply_accessibility_action(const AccessibilityAction& action) {
    ui::Node* target = find_node_by_id(root, action.node);
    if (target == nullptr) {
        return; // the node left the scene since the snapshot was published
    }
    // `Press` is the accessibility activation path (AT-SPI `DoAction`, UIA `Invoke`): it dispatches
    // the same Click a pointer or Enter/Space would, with ClickSource::Accessibility. `Focus` is
    // the
    // ordinary programmatic focus request, so screen-reader focus and keyboard focus stay one
    // state.
    //
    // `SetValue` is the "replace this field's text" path (Android `ACTION_SET_TEXT`, UIA
    // `ValuePattern.SetValue`, AT-SPI `SetValue`). The scene does not own the buffer - the app's
    // edit runtime does - so this dispatches one `TextChange` carrying the requested text and lets
    // the runtime adopt it, exactly as a committed edit would. `Scroll` stays unwired.
    if (a11y::has_action(action.action, a11y::Action::SetValue)) {
        dispatch_text_value(*target, action.text);
        return;
    }
    if (a11y::has_action(action.action, a11y::Action::Press)) {
        (void)pump.router().invoke(root, *target);
        loop->mark_render_dirty();
        return;
    }
    if (a11y::has_action(action.action, a11y::Action::Focus)) {
        if (pump.router().focus_manager().request_focus(target)) {
            loop->mark_render_dirty();
        }
    }
}

} // namespace tessera::app
